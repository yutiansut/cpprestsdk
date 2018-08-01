/***
* Copyright (C) Microsoft. All rights reserved.
* Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
*
* =+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
**/
#include "stdafx.h"

#if !defined(CPPREST_EXCLUDE_WEBSOCKETS) || !defined(_WIN32)
#include "pplx/threadpool.h"

#include <boost/asio/detail/thread.hpp>
#include <vector>

#if defined(__ANDROID__)
#include <android/log.h>
#include <jni.h>
#endif

namespace
{

struct threadpool_impl final : crossplat::threadpool
{
    threadpool_impl(size_t n)
        : crossplat::threadpool(n)
        , m_work(m_service)
    {
        for (size_t i = 0; i < n; i++)
            add_thread();
    }

    ~threadpool_impl()
    {
        m_service.stop();
        for (auto iter = m_threads.begin(); iter != m_threads.end(); ++iter)
        {
            (*iter)->join();
        }
    }

private:
    void add_thread()
    {
        m_threads.push_back(std::unique_ptr<boost::asio::detail::thread>(new boost::asio::detail::thread([&]{ thread_start(this); })));
    }

#if defined(__ANDROID__)
    static void detach_from_java(void*)
    {
        crossplat::JVM.load()->DetachCurrentThread();
    }
#endif

    static void* thread_start(void *arg) CPPREST_NOEXCEPT
    {
#if defined(__ANDROID__)
        // Calling get_jvm_env() here forces the thread to be attached.
        crossplat::get_jvm_env();
        pthread_cleanup_push(detach_from_java, nullptr);
#endif
        threadpool_impl* _this = reinterpret_cast<threadpool_impl*>(arg);
        _this->m_service.run();
#if defined(__ANDROID__)
        pthread_cleanup_pop(true);
#endif
        return arg;
    }

    std::vector<std::unique_ptr<boost::asio::detail::thread>> m_threads;
    boost::asio::io_service::work m_work;
};

#if defined(_WIN32)
// if linked into a DLL, the threadpool shared instance will be destroyed at DLL_PROCESS_DETACH,
// at which stage joining threads causes deadlock, hence this dance
static bool terminate_threads = false;
static struct restore_terminate_threads
{
    ~restore_terminate_threads()
    {
        boost::asio::detail::thread::set_terminate_threads(terminate_threads);
    }
} destroyed_after;
#endif
static std::unique_ptr<threadpool_impl> s_shared_impl_memory;
static std::atomic<threadpool_impl*> s_shared_impl(nullptr);
#if defined(_WIN32)
static struct enforce_terminate_threads
{
    ~enforce_terminate_threads()
    {
        terminate_threads = boost::asio::detail::thread::terminate_threads();
        boost::asio::detail::thread::set_terminate_threads(true);
    }
} destroyed_before;
#endif
static std::mutex s_shared_impl_mutex;

static crossplat::threadpool& get_shared_impl()
{
    auto p = s_shared_impl.load();
    if (p) return *p;
    std::lock_guard<std::mutex> lk(s_shared_impl_mutex);
    p = s_shared_impl.load();
    if (p) return *p;
    s_shared_impl_memory = utility::details::make_unique<threadpool_impl>(40);
    s_shared_impl = s_shared_impl_memory.get();
    return *s_shared_impl_memory;
}

}

namespace crossplat
{
#if defined(__ANDROID__)
// This pointer will be 0-initialized by default (at load time).
std::atomic<JavaVM*> JVM;

static void abort_if_no_jvm()
{
    if (JVM == nullptr)
    {
        __android_log_print(ANDROID_LOG_ERROR, "CPPRESTSDK", "%s", "The CppREST SDK must be initialized before first use on android: https://github.com/Microsoft/cpprestsdk/wiki/How-to-build-for-Android");
        std::abort();
    }
}

JNIEnv* get_jvm_env()
{
    abort_if_no_jvm();
    JNIEnv* env = nullptr;
    auto result = JVM.load()->AttachCurrentThread(&env, nullptr);
    if (result != JNI_OK)
    {
        throw std::runtime_error("Could not attach to JVM");
    }

    return env;
}

threadpool& threadpool::shared_instance()
{
    abort_if_no_jvm();
    return get_shared_impl();
}

#else

// initialize the static shared threadpool
threadpool& threadpool::shared_instance()
{
    return get_shared_impl();
}

#endif

void threadpool::initialize_with_threads(size_t num_threads)
{
    std::lock_guard<std::mutex> lk(s_shared_impl_mutex);
    if (s_shared_impl.load()) throw std::runtime_error("the cpprestsdk threadpool has already been initialized");
    s_shared_impl_memory = utility::details::make_unique<threadpool_impl>(num_threads);
    s_shared_impl = s_shared_impl_memory.get();
}

}

#if defined(__ANDROID__)
void cpprest_init(JavaVM* vm) {
    crossplat::JVM = vm;
}
#endif

std::unique_ptr<crossplat::threadpool> crossplat::threadpool::construct(size_t num_threads)
{
    return std::unique_ptr<crossplat::threadpool>(new threadpool_impl(num_threads));
}
#endif
