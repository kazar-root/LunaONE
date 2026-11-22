// SPDX-License-Identifier: GPL-3.0-only

#include "bridge.hpp"
#include "core/http_client.hpp"
#include "core/runtime_journal.hpp"

#include <algorithm>
#include <chrono>
#include <codecvt>
#include <fstream>
#include <locale>
#include <stdexcept>

namespace
{
JavaVM* vm = nullptr;
jclass http_class = nullptr;
jmethodID request = nullptr;
jmethodID measure = nullptr;
using Codec = std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>;

class Environment
{
  public:
    Environment()
    {
        if (!vm)
            throw std::runtime_error("Android JVM indisponible.");
        const auto result = vm->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6);
        if (result == JNI_EDETACHED)
        {
            if (vm->AttachCurrentThread(&env_, nullptr) != JNI_OK)
                throw std::runtime_error("Impossible d’utiliser le réseau Android.");
            attached_ = true;
        }
        else if (result != JNI_OK)
            throw std::runtime_error("Impossible d’utiliser le réseau Android.");
        if (env_->PushLocalFrame(32) != JNI_OK)
        {
            env_->ExceptionClear();
            if (attached_)
                vm->DetachCurrentThread();
            throw std::runtime_error("Mémoire JNI indisponible.");
        }
    }

    ~Environment()
    {
        env_->PopLocalFrame(nullptr);
        if (attached_)
            vm->DetachCurrentThread();
    }

    JNIEnv* Get() const { return env_; }

  private:
    JNIEnv* env_{};
    bool attached_ = false;
};

std::string Bytes(JNIEnv* env, jbyteArray input)
{
    if (!input)
        return {};
    std::string result(static_cast<std::size_t>(env->GetArrayLength(input)), '\0');
    if (!result.empty())
        env->GetByteArrayRegion(input, 0, static_cast<jsize>(result.size()), reinterpret_cast<jbyte*>(result.data()));
    lunaone::android::CheckJava(env, "Réponse HTTPS Android illisible.");
    return result;
}
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* loaded, void*)
{
    vm = loaded;
    return JNI_VERSION_1_6;
}

namespace lunaone::android
{
void CheckJava(JNIEnv* env, const char* message)
{
    if (env->ExceptionCheck())
    {
        env->ExceptionClear();
        throw std::runtime_error(message);
    }
}

std::string Utf8(JNIEnv* env, jstring value)
{
    if (!value)
        return {};
    std::u16string units(static_cast<std::size_t>(env->GetStringLength(value)), u'\0');
    if (!units.empty())
        env->GetStringRegion(value, 0, static_cast<jsize>(units.size()), reinterpret_cast<jchar*>(units.data()));
    CheckJava(env, "Texte Android illisible.");
    return Codec{}.to_bytes(units);
}

jstring Text(JNIEnv* env, std::string_view value)
{
    const auto units = value.empty() ? std::u16string{} : Codec{}.from_bytes(value.data(), value.data() + value.size());
    const auto result = env->NewString(reinterpret_cast<const jchar*>(units.data()), static_cast<jsize>(units.size()));
    CheckJava(env, "Mémoire JNI indisponible.");
    return result;
}

void InitializeHttp(JNIEnv* env)
{
    if (http_class)
        return;
    const auto type = env->FindClass("app/lunacore/android/AndroidHttp");
    CheckJava(env, "Client HTTPS Android introuvable.");
    request = env->GetStaticMethodID(type, "request",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;[BII)[[B");
    CheckJava(env, "Client HTTPS Android incompatible.");
    measure = env->GetStaticMethodID(type, "measureTcp", "(Ljava/lang/String;I)I");
    CheckJava(env, "Sonde réseau Android incompatible.");
    http_class = static_cast<jclass>(env->NewGlobalRef(type));
    CheckJava(env, "Mémoire JNI indisponible.");
}
}

namespace lunanx
{
HttpResponse HttpClient::Request(const std::string& method, const std::string& url, const std::string& agent,
    const std::vector<std::string>& headers, const std::string& body, long connect_seconds, long total_seconds) const
{
    using namespace lunaone::android;
    const auto start = std::chrono::steady_clock::now();
    const auto safe_url = SanitizeRuntimeUrl(url);
    const auto operation = BeginRuntimeOperation("http", method, "url=" + safe_url);
    try
    {
        Environment environment;
        auto* env = environment.Get();
        const auto string_class = env->FindClass("java/lang/String");
        CheckJava(env, "Type String Android introuvable.");
        const auto java_headers = env->NewObjectArray(static_cast<jsize>(headers.size()), string_class, nullptr);
        CheckJava(env, "Mémoire JNI indisponible.");
        for (std::size_t i = 0; i < headers.size(); ++i)
        {
            const auto value = Text(env, headers[i]);
            env->SetObjectArrayElement(java_headers, static_cast<jsize>(i), value);
            env->DeleteLocalRef(value);
            CheckJava(env, "En-tête HTTPS Android invalide.");
        }
        const auto java_body = env->NewByteArray(static_cast<jsize>(body.size()));
        CheckJava(env, "Mémoire JNI indisponible.");
        if (!body.empty())
            env->SetByteArrayRegion(java_body, 0, static_cast<jsize>(body.size()), reinterpret_cast<const jbyte*>(body.data()));
        CheckJava(env, "Corps de requête HTTPS Android invalide.");
        const auto response = static_cast<jobjectArray>(env->CallStaticObjectMethod(http_class, request,
            Text(env, method), Text(env, url), Text(env, agent), java_headers, java_body,
            static_cast<jint>(std::clamp(connect_seconds, 1L, 120L) * 1000),
            static_cast<jint>(std::clamp(total_seconds, 1L, 120L) * 1000)));
        CheckJava(env, "La connexion HTTPS Android a échoué.");
        const auto field = [&](jsize index) {
            auto value = static_cast<jbyteArray>(env->GetObjectArrayElement(response, index));
            CheckJava(env, "Réponse HTTPS Android illisible.");
            return Bytes(env, value);
        };
        HttpResponse result{std::stol(field(0)), field(1), field(2)};
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        EndRuntimeOperation(operation, "http", method, "ok", "url=" + safe_url + " status=" +
            std::to_string(result.status_code) + " bytes=" + std::to_string(result.body.size()) +
            " elapsed_ms=" + std::to_string(elapsed));
        return result;
    }
    catch (const std::exception& ex)
    {
        EndRuntimeOperation(operation, "http", method, "error", ex.what());
        throw;
    }
}

HttpResponse HttpClient::Get(const std::string& url, const std::string& agent,
    const std::vector<std::string>& headers, long connect_seconds, long total_seconds) const
{
    return Request("GET", url, agent, headers, {}, connect_seconds, total_seconds);
}

HttpResponse HttpClient::Post(const std::string& url, const std::string& agent,
    const std::vector<std::string>& headers, const std::string& body) const
{
    return Request("POST", url, agent, headers, body);
}

void HttpClient::DownloadToFile(const std::string& url, const std::string& destination, const std::string& agent,
    const std::vector<std::string>& headers, const std::function<void(long long, long long)>& progress,
    const std::function<bool()>& cancelled) const
{
    if (cancelled && cancelled())
        throw std::runtime_error("Téléchargement annulé.");
    const auto result = Get(url, agent, headers, 10, 120);
    if (cancelled && cancelled())
        throw std::runtime_error("Téléchargement annulé.");
    if (result.status_code < 200 || result.status_code >= 300)
        throw std::runtime_error("Téléchargement HTTPS refusé.");
    std::ofstream output(destination, std::ios::binary);
    if (!output.write(result.body.data(), static_cast<std::streamsize>(result.body.size())))
        throw std::runtime_error("Écriture du téléchargement impossible.");
    if (progress)
        progress(result.body.size(), result.body.size());
}

int HttpClient::MeasureTcpHandshakeLatencyMs(const std::string& url, long timeout_ms) const noexcept
{
    try
    {
        Environment environment;
        auto* env = environment.Get();
        const auto result = env->CallStaticIntMethod(http_class, measure, lunaone::android::Text(env, url),
                                                      static_cast<jint>(std::clamp(timeout_ms, 1L, 30'000L)));
        lunaone::android::CheckJava(env, "Sonde TCP Android indisponible.");
        return result;
    }
    catch (...) { return -1; }
}
} // namespace lunanx
