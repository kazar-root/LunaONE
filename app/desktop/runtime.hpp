// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/app_paths.hpp"

#include <curl/curl.h>

#include <filesystem>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lunaone::desktop
{
class CurlRuntime
{
  public:
    CurlRuntime()
    {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            throw std::runtime_error("Could not initialize libcurl.");
    }

    ~CurlRuntime() { curl_global_cleanup(); }

    CurlRuntime(const CurlRuntime&) = delete;
    CurlRuntime& operator=(const CurlRuntime&) = delete;
};

// Device registration and cookie refresh write the same files.
class AccountLock
{
  public:
    AccountLock()
    {
#ifndef _WIN32
        umask(0077);
#endif
        lunanx::PrepareAppStorage();
#ifdef _WIN32
        const auto lock_path = std::filesystem::path(lunanx::AppHomePath()) / "account.lock";
        handle_ = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Could not lock LunaONE account storage.");
#else
        descriptor_ = open(lunanx::AppHomePath().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (descriptor_ < 0)
            throw std::runtime_error("Could not open LunaONE account storage.");
        if (flock(descriptor_, LOCK_EX | LOCK_NB) != 0)
        {
            close(descriptor_);
            throw std::runtime_error("Could not lock account storage. Another LunaONE command may be running.");
        }
#endif
    }

    ~AccountLock()
    {
#ifdef _WIN32
        CloseHandle(handle_);
#else
        close(descriptor_);
#endif
    }

    AccountLock(const AccountLock&) = delete;
    AccountLock& operator=(const AccountLock&) = delete;

  private:
#ifdef _WIN32
    HANDLE handle_;
#else
    int descriptor_;
#endif
};
} // namespace lunaone::desktop
