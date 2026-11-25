// SPDX-License-Identifier: GPL-3.0-only

#include "bridge.hpp"
#include "player.hpp"
#include "core/app_paths.hpp"
#include "core/i18n.hpp"
#include "core/playback_preferences.hpp"
#include "input/controller_labels.hpp"
#include "core/update_check.hpp"
#include "core/runtime_journal.hpp"
#include "luna/luna_client.hpp"
#include "ui/device_qr.hpp"
#include "ui/image_url_policy.hpp"
#include "ui/library_sections.hpp"

#include <fstream>
#include <android/native_window_jni.h>

#include <cstdlib>
#include <thread>
#include <mutex>
#include <atomic>
#include <filesystem>
#include <limits>
#include <memory>
#include <set>
#include <utility>

namespace
{
using namespace lunanx::luna;
using namespace lunaone::android;

// Shared so that the exploration thread keeps the client it started with.
std::shared_ptr<LunaClient> client;
std::vector<Game> catalog;
std::vector<Game> visible;
std::vector<Shelf> shelves;

// The home page lists only part of Luna. The linked pages are explored on a
// thread of their own; MainActivity.catalogGrown() then asks for the games
// found, which join the catalog on the engine thread.
std::mutex explored_mutex;
std::vector<Game> explored;
std::shared_ptr<std::atomic<bool>> exploring;
JavaVM* java = nullptr;
jclass activity_class = nullptr;
jmethodID catalog_grown = nullptr;

void StopExploring()
{
    if (exploring) exploring->store(false);
    exploring.reset();
}

void StartExploring(JNIEnv* env)
{
    StopExploring();
    if (!client) return;
    if (!java && env->GetJavaVM(&java) != JNI_OK) return;
    if (!activity_class)
    {
        auto local = env->FindClass("app/lunacore/android/MainActivity");
        if (!local) { env->ExceptionClear(); return; }
        activity_class = static_cast<jclass>(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
        catalog_grown = env->GetStaticMethodID(activity_class, "catalogGrown", "()V");
        if (!catalog_grown) { env->ExceptionClear(); return; }
    }
    auto running = std::make_shared<std::atomic<bool>>(true);
    exploring = running;
    std::set<std::string> known;
    for (const auto& game : catalog) InsertGameKeys(known, game);
    std::thread([explorer = client, running, known = std::move(known)]() mutable {
        JNIEnv* thread_env = nullptr;
        const bool attached = java->AttachCurrentThread(&thread_env, nullptr) == JNI_OK;
        try
        {
            explorer->ExploreCatalog(std::move(known), [&](std::vector<Game> batch) {
                {
                    std::lock_guard lock(explored_mutex);
                    for (auto& game : batch) explored.push_back(std::move(game));
                }
                if (attached && running->load())
                {
                    thread_env->CallStaticVoidMethod(activity_class, catalog_grown);
                    thread_env->ExceptionClear();
                }
            }, [running] { return running->load(); });
        }
        catch (const std::exception&) {}
        if (attached) java->DetachCurrentThread();
    }).detach();
}

// Engine thread: adds the games found so far.
void MergeExplored()
{
    std::vector<Game> batch;
    {
        std::lock_guard lock(explored_mutex);
        batch.swap(explored);
    }
    if (batch.empty()) return;
    std::set<std::string> known;
    for (const auto& game : catalog) InsertGameKeys(known, game);
    for (auto& game : batch)
        if (InsertGameKeys(known, game)) catalog.push_back(std::move(game));
}

class Job
{
  public:
    Job(JNIEnv* env, jobject object) : env_(env), object_(object)
    {
        auto type = env_->GetObjectClass(object_);
        cancelled_ = env_->GetFieldID(type, "cancelled", "Z");
        CheckJava(env_, "Tâche Android incompatible.");
        status_ = env_->GetMethodID(type, "onStatus", "(Ljava/lang/String;)V");
        CheckJava(env_, "Tâche Android incompatible.");
        code_ = env_->GetMethodID(type, "onCode", "(Ljava/lang/String;Ljava/lang/String;)V");
        CheckJava(env_, "Tâche Android incompatible.");
        metrics_ = env_->GetMethodID(type, "onMetrics", "(Ljava/lang/String;)V");
        CheckJava(env_, "Tâche Android incompatible.");
        env_->DeleteLocalRef(type);
    }

    bool Cancelled() const { return env_->GetBooleanField(object_, cancelled_); }

    void Status(const std::string& message) const
    {
        const auto text = Text(env_, lunanx::Tr(message));
        env_->CallVoidMethod(object_, status_, text);
        env_->DeleteLocalRef(text);
        CheckJava(env_, "État Android indisponible.");
    }

    void Code(const DeviceCodeChallenge& challenge) const
    {
        const auto code = Text(env_, challenge.user_code);
        const auto url = Text(env_, challenge.verification_url);
        env_->CallVoidMethod(object_, code_, code, url);
        env_->DeleteLocalRef(code);
        env_->DeleteLocalRef(url);
        CheckJava(env_, "Page de connexion Android indisponible.");
    }

    void Metrics(const std::string& message) const
    {
        const auto text = Text(env_, message);
        env_->CallVoidMethod(object_, metrics_, text);
        env_->DeleteLocalRef(text);
        CheckJava(env_, "État Android indisponible.");
    }

  private:
    JNIEnv* env_;
    jobject object_;
    jfieldID cancelled_{};
    jmethodID status_{}, code_{}, metrics_{};
};

void Throw(JNIEnv* env, const std::exception& error) noexcept
{
    env->ExceptionClear();
    const auto type = env->FindClass("java/lang/IllegalStateException");
    if (!type)
        return;
    try
    {
        const auto ctor = env->GetMethodID(type, "<init>", "(Ljava/lang/String;)V");
        CheckJava(env, "Erreur Android.");
        const auto object = static_cast<jthrowable>(env->NewObject(type, ctor, Text(env, error.what())));
        if (object)
            env->Throw(object);
    }
    catch (...)
    {
        env->ThrowNew(type, "Erreur LunaONE.");
    }
}

template <class Action>
jobjectArray RunArray(JNIEnv* env, Action&& action) noexcept
{
    try { return action(); }
    catch (const std::exception& error) { Throw(env, error); return nullptr; }
}

jobjectArray Strings(JNIEnv* env, std::size_t count)
{
    if (count > std::numeric_limits<jsize>::max())
        throw std::runtime_error("Catalogue Android trop volumineux.");
    const auto type = env->FindClass("java/lang/String");
    CheckJava(env, "Type String Android introuvable.");
    auto result = env->NewObjectArray(static_cast<jsize>(count), type, nullptr);
    CheckJava(env, "Mémoire JNI indisponible.");
    return result;
}

void Set(JNIEnv* env, jobjectArray array, std::size_t index, const std::string& value)
{
    auto entry = Text(env, value);
    env->SetObjectArrayElement(array, static_cast<jsize>(index), entry);
    env->DeleteLocalRef(entry);
    CheckJava(env, "Catalogue Android inaccessible.");
}

jobjectArray Display(JNIEnv* env, std::vector<Game> games)
{
    constexpr std::size_t header_size = 3, game_size = 6;
    const auto plan = client->Plan();
    const auto profile = client->Profile();
    auto result = Strings(env, header_size + games.size() * game_size);
    Set(env, result, 0, plan == AccountPlan::Premium ? "Luna Premium" :
                            plan == AccountPlan::Standard ? "Luna Standard" : client->PlanLabel());
    Set(env, result, 1, profile.name);
    Set(env, result, 2, profile.avatar_url);
    for (std::size_t i = 0; i < games.size(); ++i)
    {
        const auto card = lunanx::ui::PresentLibraryGame(games[i], plan);
        const auto offset = header_size + i * game_size;
        Set(env, result, offset, games[i].title);
        Set(env, result, offset + 1, lunanx::ResizeAmazonImageUrl(card.cover, 360));
        Set(env, result, offset + 2, lunanx::ResizeAmazonImageUrl(card.backdrop, 1280));
        Set(env, result, offset + 3, card.offer);
        Set(env, result, offset + 4, card.metadata);
        Set(env, result, offset + 5, card.locked ? "locked" : card.included ? "included" : "");
    }
    visible = std::move(games);
    return result;
}

void RequireClient()
{
    if (!client)
        throw std::runtime_error("Connecte ton compte Amazon pour utiliser le catalogue.");
}

jobjectArray Restore(JNIEnv* env)
{
    DeviceRegistration registration;
    if (!AmazonAuth::Load(registration))
    {
        client.reset();
        catalog.clear();
        visible.clear();
        shelves.clear();
        lunanx::SetUiLanguageForMarket(lunanx::LoadPlaybackPreferences().market);
        if (std::filesystem::exists(AmazonAuth::StoragePath()))
            throw std::runtime_error("Connexion enregistrée illisible. Déconnecte-toi, puis crée un code.");
        return nullptr;
    }
    lunanx::SetUiLanguageForMarket(registration.market_code);
    CookieJar cookies;
    cookies.SetCookies(registration.cookies);
    auto next_client = std::make_unique<LunaClient>(std::move(cookies), std::move(registration));
    next_client->SignIn();
    std::vector<Game> next_catalog;
    std::set<std::string> known;
    auto sections = next_client->FetchShelves();
    for (const auto& shelf : sections)
        for (const auto& game : shelf.games)
            if (InsertGameKeys(known, game))
                next_catalog.push_back(game);
    StopExploring();
    {
        std::lock_guard lock(explored_mutex);
        explored.clear();
    }
    client = std::move(next_client);
    catalog = std::move(next_catalog);
    shelves = std::move(sections);
    StartExploring(env);
    return Display(env, lunanx::ui::LibraryGames(lunanx::ui::LibrarySection::All, shelves, catalog, client->Plan()));
}
} // namespace

extern "C" JNIEXPORT void JNICALL Java_app_lunacore_android_MainActivity_nativeConfigure(
    JNIEnv* env, jclass, jstring path)
{
    try
    {
        InitializeHttp(env);
        static const bool configured = [&] {
            if (setenv("LUNACORE_DATA_DIR", Utf8(env, path).c_str(), 1) != 0)
                throw std::runtime_error("Impossible de configurer le dossier privé Android.");
            lunanx::PrepareAppStorage();
            DeviceRegistration registration;
            if (AmazonAuth::Load(registration))
                lunanx::SetUiLanguageForMarket(registration.market_code);
            else
                lunanx::SetUiLanguageForMarket(lunanx::LoadPlaybackPreferences().market);
            lunanx::InitializeRuntimeJournal("LunaONE-" + std::string(lunanx::InstalledVersion()) + "-android");
            return true;
        }();
        static_cast<void>(configured);
    }
    catch (const std::exception& error) { Throw(env, error); }
}

extern "C" JNIEXPORT jobjectArray JNICALL Java_app_lunacore_android_MainActivity_nativeMarkets(JNIEnv* env, jclass)
{
    return RunArray(env, [&] {
        auto result = Strings(env, AmazonMarkets().size() * 2);
        for (std::size_t i = 0; i < AmazonMarkets().size(); ++i)
        {
            Set(env, result, 2 * i, AmazonMarkets()[i].code);
            Set(env, result, 2 * i + 1, AmazonMarkets()[i].native_name);
        }
        return result;
    });
}

extern "C" JNIEXPORT jboolean JNICALL Java_app_lunacore_android_MainActivity_nativeHasRegistration(JNIEnv* env, jclass)
{
    try { return std::filesystem::exists(AmazonAuth::StoragePath()); }
    catch (const std::exception& error) { Throw(env, error); return JNI_FALSE; }
}

extern "C" JNIEXPORT jobjectArray JNICALL Java_app_lunacore_android_MainActivity_nativeRestore(JNIEnv* env, jclass)
{
    return RunArray(env, [&] { return Restore(env); });
}

extern "C" JNIEXPORT jobjectArray JNICALL Java_app_lunacore_android_MainActivity_nativeConnect(
    JNIEnv* env, jclass, jstring market, jobject listener)
{
    return RunArray(env, [&]() -> jobjectArray {
        if (std::filesystem::exists(AmazonAuth::StoragePath()))
            throw std::runtime_error("Un compte est déjà enregistré sur cet appareil.");
        Job job(env, listener);
        if (job.Cancelled())
            return nullptr;
        auto device = AmazonAuth::SignInWithCode(Utf8(env, market), [&](const auto& code) { job.Code(code); },
                                                 [&] { return job.Cancelled(); });
        try
        {
            if (job.Cancelled())
                throw std::runtime_error("Connexion annulée.");
            if (!AmazonAuth::Save(device))
                throw std::runtime_error("Impossible d’enregistrer la connexion Amazon.");
        }
        catch (...)
        {
            try { AmazonAuth::Deregister(device); } catch (...) {}
            throw;
        }
        job.Status("Chargement du catalogue Luna…");
        return Restore(env);
    });
}

extern "C" JNIEXPORT jobjectArray JNICALL Java_app_lunacore_android_MainActivity_nativeSearch(
    JNIEnv* env, jclass, jstring query, jobject listener)
{
    return RunArray(env, [&] {
        RequireClient();
        Job job(env, listener);
        auto found = client->SearchGames(Utf8(env, query), [&] { return !job.Cancelled(); });
        return job.Cancelled() ? nullptr : Display(env, std::move(found));
    });
}

extern "C" JNIEXPORT jobjectArray JNICALL Java_app_lunacore_android_MainActivity_nativeCatalog(
    JNIEnv* env, jclass, jobject listener, jint section, jboolean explore)
{
    return RunArray(env, [&] {
        RequireClient();
        MergeExplored();
        if (explore)
        {
            Job job(env, listener);
            std::set<std::string> known;
            for (const auto& game : catalog)
                InsertGameKeys(known, game);
            client->ExploreCatalog(std::move(known), [&](std::vector<Game> batch) {
                for (auto& game : batch)
                    catalog.push_back(std::move(game));
                job.Status(lunanx::TrFormat("{} titres trouvés…", {std::to_string(catalog.size())}));
            }, [&] { return !job.Cancelled(); });
        }
        return Display(env, lunanx::ui::LibraryGames(static_cast<lunanx::ui::LibrarySection>(section), shelves, catalog, client->Plan()));
    });
}

// « Tous les jeux » with the games the exploration has found so far.
extern "C" JNIEXPORT jobjectArray JNICALL Java_app_lunacore_android_MainActivity_nativeExploredCatalog(
    JNIEnv* env, jclass, jint section)
{
    return RunArray(env, [&]() -> jobjectArray {
        if (!client) return nullptr;
        MergeExplored();
        return Display(env, lunanx::ui::LibraryGames(static_cast<lunanx::ui::LibrarySection>(section), shelves, catalog,
                                                     client->Plan()));
    });
}

extern "C" JNIEXPORT void JNICALL Java_app_lunacore_android_MainActivity_nativeSignOut(JNIEnv* env, jclass)
{
    try
    {
        DeviceRegistration registration;
        if (AmazonAuth::Load(registration))
            try { AmazonAuth::Deregister(registration); } catch (const std::exception&) {}
        if (!AmazonAuth::EraseAccountData())
            throw std::runtime_error("Impossible d’effacer la connexion enregistrée.");
        StopExploring();
        client.reset();
        catalog.clear();
        visible.clear();
        shelves.clear();
        lunanx::SetUiLanguageForMarket(lunanx::LoadPlaybackPreferences().market);
    }
    catch (const std::exception& error) { Throw(env, error); }
}

extern "C" JNIEXPORT void JNICALL Java_app_lunacore_android_MainActivity_nativePlay(
    JNIEnv* env, jclass activity, jint index, jobject surface, jobject listener, jboolean can_choose_decoder)
{
    try
    {
        RequireClient();
        if (index < 0 || static_cast<std::size_t>(index) >= visible.size())
            throw std::runtime_error("Ce jeu ne figure plus dans les résultats.");
        Job job(env, listener);
        if (job.Cancelled())
            return;
        const auto window = std::unique_ptr<ANativeWindow, decltype(&ANativeWindow_release)>(
            ANativeWindow_fromSurface(env, surface), &ANativeWindow_release);
        CheckJava(env, "Surface Android indisponible.");
        if (!window)
            throw std::runtime_error("Surface Android indisponible.");
        // The automatic mode's values when it is on.
        const auto settings = lunanx::ResolvedPlaybackPreferences(lunanx::LoadPlaybackPreferences());
        std::string decoder;
        if (can_choose_decoder && settings.decoder != lunanx::PlaybackDecoder::Automatic)
        {
            const auto find_decoder = env->GetStaticMethodID(activity, "findAvcDecoder", "(III)Ljava/lang/String;");
            CheckJava(env, "Sélection du décodeur Android indisponible.");
            const auto profile = lunanx::PlaybackDisplay(settings);
            const auto name = static_cast<jstring>(env->CallStaticObjectMethod(activity, find_decoder,
                static_cast<jint>(settings.decoder), profile.width, profile.height));
            CheckJava(env, "Impossible de détecter les décodeurs Android.");
            if (!name)
                throw std::runtime_error(lunanx::Tr(settings.decoder == lunanx::PlaybackDecoder::Hardware
                    ? "Aucun décodeur H.264 matériel compatible avec cette résolution."
                    : "Aucun décodeur H.264 logiciel compatible avec cette résolution."));
            decoder = Utf8(env, name);
            env->DeleteLocalRef(name);
        }
        // The game gets the whole connection; the exploration resumes after it.
        const bool resume = exploring && exploring->load();
        StopExploring();
        struct Resume
        {
            JNIEnv* env;
            bool on;
            ~Resume() { if (on) StartExploring(env); }
        } after{env, resume};
        Play(*client, visible[index], window.get(), settings, decoder, [&] { return job.Cancelled(); },
             [&](const auto& message) { job.Status(message); }, [&](const auto& message) { job.Metrics(message); });
    }
    catch (const std::exception& error) { Throw(env, error); }
}

extern "C" JNIEXPORT jstring JNICALL Java_app_lunacore_android_MainActivity_nativeText(
    JNIEnv* env, jclass, jstring key)
{
    try { return Text(env, lunanx::Tr(Utf8(env, key))); }
    catch (const std::exception& error) { Throw(env, error); return nullptr; }
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_app_lunacore_android_MainActivity_nativeQr(
    JNIEnv* env, jclass, jstring address)
{
    try
    {
        const auto qr = lunanx::ui::MakeDeviceQr(Utf8(env, address));
        auto buffer = env->NewByteArray(static_cast<jsize>(qr.dark.size()));
        if (buffer && !qr.dark.empty())
            env->SetByteArrayRegion(buffer, 0, static_cast<jsize>(qr.dark.size()), reinterpret_cast<const jbyte*>(qr.dark.data()));
        return buffer;
    }
    catch (const std::exception& error) { Throw(env, error); return nullptr; }
}

extern "C" JNIEXPORT jobjectArray JNICALL Java_app_lunacore_android_MainActivity_nativePlaybackChoices(JNIEnv* env, jclass, jboolean can_choose_decoder)
{
    try
    {
        // Per row: section title, title, selected index, label count, labels.
        const auto choices = lunanx::PlaybackChoices(lunanx::LoadPlaybackPreferences(), can_choose_decoder);
        std::size_t count = 0;
        for (const auto& choice : choices) count += 4 + choice.labels.size();
        const auto array = Strings(env, count);
        std::size_t i = 0;
        for (const auto& choice : choices)
        {
            Set(env, array, i++, lunanx::Tr(lunanx::PlaybackSectionTitle(choice.section)));
            Set(env, array, i++, choice.title);
            Set(env, array, i++, std::to_string(choice.selected));
            Set(env, array, i++, std::to_string(choice.labels.size()));
            for (const auto& label : choice.labels) Set(env, array, i++, label);
        }
        return array;
    }
    catch (const std::exception& error) { Throw(env, error); return nullptr; }
}

// The screen in physical pixels, for the automatic video mode.
extern "C" JNIEXPORT void JNICALL Java_app_lunacore_android_MainActivity_nativeSetDisplay(
    JNIEnv*, jclass, jint height, jint refresh)
{
    lunanx::SetDeviceDisplay({height, refresh});
}

// Controller selection and disconnection, for the runtime journal.
extern "C" JNIEXPORT void JNICALL Java_app_lunacore_android_MainActivity_nativeLog(
    JNIEnv* env, jclass, jstring event, jstring detail)
{
    try { lunanx::LogRuntimeEvent("android", Utf8(env, event), Utf8(env, detail)); }
    catch (const std::exception&) {}
}

// Bit 1: A/B swapped, bit 2: X/Y swapped, bit 4: left stick Y inverted.
extern "C" JNIEXPORT jint JNICALL Java_app_lunacore_android_MainActivity_nativeControllerOptions(JNIEnv* env, jclass)
{
    try
    {
        const auto preferences = lunanx::LoadPlaybackPreferences();
        return (preferences.swap_ab ? 1 : 0) | (preferences.swap_xy ? 2 : 0) | (preferences.invert_left_y ? 4 : 0);
    }
    catch (const std::exception& error) { Throw(env, error); return 0; }
}

extern "C" JNIEXPORT jboolean JNICALL Java_app_lunacore_android_MainActivity_nativeVirtualKeyboard(JNIEnv* env, jclass)
{
    try { return lunanx::LoadPlaybackPreferences().virtual_keyboard; }
    catch (const std::exception& error) { Throw(env, error); return JNI_FALSE; }
}

// The controller shortcuts, named as printed on the controller (USB vendor id).
extern "C" JNIEXPORT jobjectArray JNICALL Java_app_lunacore_android_MainActivity_nativeShortcutNotes(
    JNIEnv* env, jclass, jint vendor)
{
    return RunArray(env, [&] {
        const auto family = lunanx::input::ControllerFamilyFromVendor(static_cast<unsigned>(vendor));
        auto result = Strings(env, 3);
        Set(env, result, 0, lunanx::TrFormat("Quitter une partie : {}", {lunanx::input::ExitChordName(family)}));
        Set(env, result, 1, lunanx::TrFormat("Clavier virtuel : {}", {lunanx::input::KeyboardChordName(family)}));
        Set(env, result, 2, lunanx::TrFormat("Infos techniques en jeu : {}", {lunanx::input::OverlayChordName(family)}));
        return result;
    });
}

extern "C" JNIEXPORT jboolean JNICALL Java_app_lunacore_android_MainActivity_nativeSelectPlaybackChoice(
    JNIEnv* env, jclass, jint row, jint choice, jboolean can_choose_decoder)
{
    try
    {
        auto prefs = lunanx::LoadPlaybackPreferences();
        lunanx::PlaybackChoices(prefs, can_choose_decoder).at(row).apply(prefs, choice);
        if (!lunanx::SavePlaybackPreferences(prefs)) return JNI_FALSE;
        DeviceRegistration registration;
        lunanx::SetUiLanguageForMarket(AmazonAuth::Load(registration) ? registration.market_code : prefs.market);
        return JNI_TRUE;
    }
    catch (const std::exception& error) { Throw(env, error); return JNI_FALSE; }
}

extern "C" JNIEXPORT jboolean JNICALL Java_app_lunacore_android_MainActivity_nativeSimpleLibrary(JNIEnv* env, jclass)
{
    try { return lunanx::LoadPlaybackPreferences().simple_library; }
    catch (const std::exception& error) { Throw(env, error); return JNI_FALSE; }
}

extern "C" JNIEXPORT jstring JNICALL Java_app_lunacore_android_MainActivity_nativePreferredMarket(JNIEnv* env, jclass)
{
    try { return Text(env, lunanx::LoadPlaybackPreferences().market); }
    catch (const std::exception& error) { Throw(env, error); return nullptr; }
}

namespace
{
// The last check's result, which the install button downloads.
std::mutex update_mutex;
lunanx::UpdateInfo last_update;
}

extern "C" JNIEXPORT jstring JNICALL Java_app_lunacore_android_MainActivity_nativeCheckUpdates(
    JNIEnv* env, jclass, jstring installed)
{
    try
    {
        auto update = installed ? lunanx::FindUpdate(Utf8(env, installed)) : lunanx::FindUpdate();
        const auto message = update.message;
        std::lock_guard lock(update_mutex);
        last_update = std::move(update);
        return Text(env, message);
    }
    catch (const std::exception& error) { Throw(env, error); return nullptr; }
}

// "V0.4" when the last check found an APK to install, otherwise empty.
extern "C" JNIEXPORT jstring JNICALL Java_app_lunacore_android_MainActivity_nativeUpdateVersion(JNIEnv* env, jclass)
{
    std::lock_guard lock(update_mutex);
    return Text(env, last_update.Downloadable() ? "V" + last_update.version : std::string());
}

// Downloads and checks the APK into `path`; returns an error message, or an
// empty text once the file is ready.
extern "C" JNIEXPORT jstring JNICALL Java_app_lunacore_android_MainActivity_nativeDownloadUpdate(
    JNIEnv* env, jclass, jstring path)
{
    try
    {
        lunanx::UpdateInfo update;
        {
            std::lock_guard lock(update_mutex);
            update = last_update;
        }
        const auto data = lunanx::DownloadUpdate(update);
        std::ofstream file(Utf8(env, path), std::ios::binary | std::ios::trunc);
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        file.close();
        if (!file) return Text(env, lunanx::Tr("Impossible de télécharger la mise à jour."));
        return Text(env, std::string());
    }
    catch (const std::exception& error) { return Text(env, error.what()); }
}

extern "C" JNIEXPORT void JNICALL Java_app_lunacore_android_MainActivity_nativeInput(
    JNIEnv*, jclass, jint buttons, jint left, jint right, jint lx, jint ly, jint rx, jint ry)
{
    UpdateGamepad({static_cast<std::uint16_t>(buttons), static_cast<std::uint8_t>(left), static_cast<std::uint8_t>(right),
                  static_cast<std::int16_t>(lx), static_cast<std::int16_t>(ly),
                  static_cast<std::int16_t>(rx), static_cast<std::int16_t>(ry)});
}
