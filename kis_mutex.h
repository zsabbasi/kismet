/*

    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/* 
 * So it turns out GCC < 4.9.0 has a broken std::timex_mutex implementation
 * which makes it unusable (well, unusable in a timed fashion).
 *
 * Since Kismet needs to support older implementations, we need to work around
 * this by detecting it and implementing our own mutex on those systems.
 */

#ifndef __KISMET_MUTEX_H__
#define __KISMET_MUTEX_H__

#include "config.h"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

#ifdef HAVE_CXX14
#include <shared_mutex>
#endif


#include <pthread.h>

#include "fmt.h"

// Seconds a lock is allowed to be held before throwing a timeout error
#define KIS_THREAD_DEADLOCK_TIMEOUT     5

// Optionally force the custom c++ workaround mutex
#define ALWAYS_USE_KISMET_MUTEX         0

// Some compilers (older openwrt CC images, Ubuntu 14.04) are still in use and have a broken
// std::recursive_timed_mutex implementation which uses the wrong precision for the timer leading
// to an instant timer failure;  Optionally re-implement a std::mutex using C pthread 
// primitives and locking
class kis_recursive_pthread_timed_mutex {
public:
    kis_recursive_pthread_timed_mutex() {
        // Make a recursive mutex that the owning thread can lock multiple times;
        // Required to allow a timer event to reschedule itself on completion
        pthread_mutexattr_t mutexattr;
        pthread_mutexattr_init(&mutexattr);
        pthread_mutexattr_settype(&mutexattr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&mutex, &mutexattr);
    }
        
    ~kis_recursive_pthread_timed_mutex() {
        pthread_mutex_destroy(&mutex);
    }

    bool try_lock_for(const std::chrono::seconds& d) {
#if defined(HAVE_PTHREAD_TIMELOCK) && !defined(DISABLE_MUTEX_TIMEOUT)
        // Only use timeouts if a) they're supported and b) not disabled in configure
        struct timespec t;

        clock_gettime(CLOCK_REALTIME, &t); 
        t.tv_sec += d.count();

        if (pthread_mutex_timedlock(&mutex, &t) != 0) {
            return false;
        }
#else
        pthread_mutex_lock(&mutex);
#endif

        return true;
    }

    void lock() {
        pthread_mutex_lock(&mutex);
    }

    void unlock() {
        pthread_mutex_unlock(&mutex);
    }

private:
    pthread_mutex_t mutex;
};

// C++14 defines a shared_mutex, and a timed shared mutex, but not a recursive, timed,
// shared mutex; implement our own thread ID 
class kis_recursive_timed_mutex {
public:
    kis_recursive_timed_mutex() :
        owner {std::thread::id()},
        owner_count {0},
        shared_owner_count {0} { }

    // Write operation; allow recursion through the owner TID, but do not
    // allow a write lock if ANY thread holds a RO lock
    bool try_lock_for(const std::chrono::seconds& d) {
        // Must wait for shared locks to release before we can acquire a write lock
        if (shared_owner_count) {
            // This will be unlocked when the shared count hits 0
            if (mutex.try_lock_for(d) == false) {
                throw(std::runtime_error(fmt::format("deadlock: mutex not available within {} (shared held)", KIS_THREAD_DEADLOCK_TIMEOUT)));
            }

            // Set the owner & count
            owner = std::this_thread::get_id();
            owner_count = 1;

            return true;
        }

        // If we're already the owner, increment the recursion counter
        if (owner_count > 0 && std::this_thread::get_id() == owner) {
            // Increment the write lock count and we're done
            owner_count++;
            return true;
        }

        // Attempt to acquire and continue
        if (mutex.try_lock_for(d) == false) {
            throw(std::runtime_error(fmt::format("deadlock: shared mutex lock not available within {} (claiming write)", KIS_THREAD_DEADLOCK_TIMEOUT)));
        }

        // Acquire the owner write lock
        owner = std::this_thread::get_id();
        owner_count = 1;

        return true;
    }

    bool try_lock_shared_for(const std::chrono::seconds& d) {
        if (owner_count > 0) {
            // Acquire a RO lock as if it were a RW lock if the thread is the owner
            if (std::this_thread::get_id() == owner) {
                owner_count++;
                return true;
            }

            // If we have any other writer lock, we must block until it's gone; the RW 
            // count hitting 0 will unlock us
            if (mutex.try_lock_for(d) == false) {
                throw(std::runtime_error(fmt::format("deadlock: shared mutex lock not available within {} (write held)", KIS_THREAD_DEADLOCK_TIMEOUT)));
            }

            // We now own the lock, increment RO
            shared_owner_count++;
            return true;
        }

        // If nobody owns it...
        if (shared_owner_count == 0) {
            // Grab the lock
            if (mutex.try_lock_for(d) == false) {
                throw(std::runtime_error(fmt::format("deadlock: shared mutex lock not available within {} (claiming shared)", KIS_THREAD_DEADLOCK_TIMEOUT)));
            }
        }

        // Increment the RO usage count
        shared_owner_count++;

        return true;
    }

    void lock() {
        // Must wait for shared locks to release before we can acquire a write lock
        if (shared_owner_count) {
            // This will be unlocked when the shared count hits 0
            mutex.lock();

            // Set the owner & count
            owner = std::this_thread::get_id();
            owner_count = 1;

            return;
        }

        // If we're already the owner, increment the recursion counter
        if (owner_count > 0 && std::this_thread::get_id() == owner) {
            // Increment the write lock count and we're done
            owner_count++;
            return;
        }

        // Attempt to acquire and continue
        mutex.lock();
        owner = std::this_thread::get_id();
        owner_count = 1;

        return;
    }

    void lock_shared() {
        if (owner_count > 0) {
            // Acquire a RO lock as if it were a RW lock if the thread is the owner
            if (std::this_thread::get_id() == owner) {
                owner_count++;
                return;
            }

            // If we have any other writer lock, we must block until it's gone; the RW 
            // count hitting 0 will unlock us
            mutex.lock();

            // We now own the lock, increment RO
            shared_owner_count++;
            return;
        }

        if (shared_owner_count == 0) {
            // Grab the lock
            mutex.lock();
        }

        // Increment the RO usage count
        shared_owner_count++;

        return;
    }

    void unlock() {
        if (owner_count > 0) {
            // Write lock has expired, unlock mutex
            if (--owner_count == 0) {
                owner = std::thread::id();
                mutex.unlock();
            }

            return;
        } else {
            throw std::runtime_error("Mutex got a write-unlock when no write lock held");
        }
    }

    void unlock_shared() {
        if (owner_count > 0) {
            // Unlock a RO held by the RW thread as a RW
            if (std::this_thread::get_id() == owner) {
                if (owner_count > 0) {
                    // Write lock has expired, unlock mutex
                    if (--owner_count == 0) {
                        owner = std::thread::id();
                        owner_count = 0;
                        mutex.unlock();
                    }

                    return;
                } else {
                    throw std::runtime_error("Mutex got a shared unlock by a write-unlock owner when no write lock held");
                }
            }

            throw std::runtime_error("Mutex got a shared-unlock when a write lock held");
        }

        if (shared_owner_count > 0) {
            // Decrement RO lock count
            if (--shared_owner_count == 0) {
                // Release the lock if we've hit 0
                mutex.unlock();
                return;
            }
        }
    }

private:
    // Recursive write lock
    std::atomic<std::thread::id> owner;
    std::atomic<unsigned int> owner_count;

    // RO shared locks
    std::atomic<unsigned int> shared_owner_count;

// Use std::recursive_timed_mutex components when we can, unless we're forcing pthread mode; base it
// on the GCC versions to detect broken compilers
#if ALWAYS_USE_KISMET_MUTEX != 0 || \
    (!defined(__clang__) && defined (GCC_VERSION_MAJOR) && (GCC_VERSION_MAJOR < 4 || \
        (GCC_VERSION_MAJOR == 4 && GCC_VERSION_MINOR < 9)))
    kis_recursive_pthread_timed_mutex mutex;
#else
    std::timed_mutex mutex;
#endif
};


// A scoped locker like std::lock_guard that provides RAII scoped locking of a kismet mutex;
// unless disabled in ./configure use a timed lock mutex and throw an exception if unable
// to acquire the lock within KIS_THREAD_DEADLOCK_TIMEOUT seconds, it's better to crash
// than to hang; we allow a short-cut unlock to unlock before the end of scope, in which case
// we no longer unlock AGAIN at descope
class local_locker {
public:
    local_locker(kis_recursive_timed_mutex *in) : 
        cpplock(in),
        hold_lock(true) {
#ifdef DISABLE_MUTEX_TIMEOUT
        cpplock->lock();
#else
        if (!cpplock->try_lock_for(std::chrono::seconds(KIS_THREAD_DEADLOCK_TIMEOUT))) {
            throw(std::runtime_error(fmt::format("deadlock: mutex not available within "
                            "{}", KIS_THREAD_DEADLOCK_TIMEOUT)));
        }
#endif
    }

    local_locker() = delete;

    void unlock() {
        hold_lock = false;
        cpplock->unlock();
    }

    ~local_locker() {
        if (hold_lock)
            cpplock->unlock();
    }

protected:
    kis_recursive_timed_mutex *cpplock;
    std::atomic<bool> hold_lock;
};

// A local RAII locker for READ ONLY access, allows us to optimize the read-only mutexes
// if we're on C++14 and above, acts like a normal mutex locker if we're on older compilers.
class local_shared_locker {
public:
    local_shared_locker(kis_recursive_timed_mutex *in) : 
        cpplock(in),
        hold_lock(true) {
#ifdef DISABLE_MUTEX_TIMEOUT
        cpplock->shared_lock();
#else
        if (!cpplock->try_lock_shared_for(std::chrono::seconds(KIS_THREAD_DEADLOCK_TIMEOUT))) {
            throw(std::runtime_error(fmt::format("deadlock: mutex not available within "
                            "{}", KIS_THREAD_DEADLOCK_TIMEOUT)));
        }
#endif
    }

    local_shared_locker() = delete;

    void unlock() {
        hold_lock = false;
        cpplock->unlock_shared();
    }

    ~local_shared_locker() {
        if (hold_lock)
            cpplock->unlock_shared();
    }

protected:
    kis_recursive_timed_mutex *cpplock;
    std::atomic<bool> hold_lock;
};


// RAII-style scoped locker, but only locks on demand, not creation
class local_demand_locker {
public:
    local_demand_locker(kis_recursive_timed_mutex *in) : 
        cpplock(in),
        hold_lock(false) { }

    void unlock() {
        if (!hold_lock)
            return;

        hold_lock = false;
        cpplock->unlock();
    }

    void lock() {
        if (hold_lock)
            throw(std::runtime_error("possible deadlock - demand_locker locking while "
                        "already holding a lock"));

        hold_lock = true;

#ifdef DISABLE_MUTEX_TIMEOUT
        cpplock->lock();
#else
        if (!cpplock->try_lock_for(std::chrono::seconds(KIS_THREAD_DEADLOCK_TIMEOUT))) {
            throw(std::runtime_error(fmt::format("deadlock: mutex not available within "
                            "{}", KIS_THREAD_DEADLOCK_TIMEOUT)));
        }
#endif
    }

    ~local_demand_locker() {
        unlock();
    }

protected:
    kis_recursive_timed_mutex *cpplock;
    std::atomic<bool> hold_lock;
};

// RAII-style scoped locker, but only locks on demand, not creation, with shared mutex
class local_shared_demand_locker {
public:
    local_shared_demand_locker(kis_recursive_timed_mutex *in) : 
        cpplock(in),
        hold_lock(false) { }

    void unlock() {
        if (!hold_lock)
            return;

        hold_lock = false;
        cpplock->unlock_shared();
    }

    void lock() {
        if (hold_lock)
            throw(std::runtime_error("possible deadlock - shared_demand_locker locking while "
                        "already holding a lock"));

        hold_lock = true;

#ifdef DISABLE_MUTEX_TIMEOUT
        cpplock->lock_shared();
#else
        if (!cpplock->try_lock_shared_for(std::chrono::seconds(KIS_THREAD_DEADLOCK_TIMEOUT))) {
            throw(std::runtime_error(fmt::format("deadlock: mutex not available within "
                            "{}", KIS_THREAD_DEADLOCK_TIMEOUT)));
        }
#endif
    }

    ~local_shared_demand_locker() {
        unlock();
    }

protected:
    kis_recursive_timed_mutex *cpplock;
    std::atomic<bool> hold_lock;
};

// Act as a scoped locker on a mutex that never expires; used for performing
// end-of-life mutex maintenance
class local_eol_locker {
public:
    local_eol_locker(kis_recursive_timed_mutex *in) {
#ifdef DISABLE_MUTEX_TIMEOUT
        in->lock();
#else
        if (!in->try_lock_for(std::chrono::seconds(KIS_THREAD_DEADLOCK_TIMEOUT))) {
            throw(std::runtime_error(fmt::format("deadlock: mutex not available within "
                            "{}", KIS_THREAD_DEADLOCK_TIMEOUT)));
        }
#endif
    }

    ~local_eol_locker() { }
};

class local_eol_shared_locker {
public:
    local_eol_shared_locker(kis_recursive_timed_mutex *in) {
#ifdef DISABLE_MUTEX_TIMEOUT
        in->lock_shared();
#else
        if (!in->try_lock_shared_for(std::chrono::seconds(KIS_THREAD_DEADLOCK_TIMEOUT))) {
            throw(std::runtime_error(fmt::format("deadlock: mutex not available within "
                            "{}", KIS_THREAD_DEADLOCK_TIMEOUT)));
        }
#endif
    }

    local_eol_shared_locker(kis_recursive_timed_mutex& in) {
#ifdef DISABLE_MUTEX_TIMEOUT
        in->lock_shared();
#else
        if (!in.try_lock_shared_for(std::chrono::seconds(KIS_THREAD_DEADLOCK_TIMEOUT))) {
            throw(std::runtime_error(fmt::format("deadlock: mutex not available within "
                            "{}", KIS_THREAD_DEADLOCK_TIMEOUT)));
        }
#endif
    }
};

// Act as a scope-based unlocker; assuming a mutex is already locked, unlock
// when it leaves scope
class local_unlocker {
public:
    local_unlocker(kis_recursive_timed_mutex *in) : cpplock(in) { }

    ~local_unlocker() {
        cpplock->unlock();
    }

protected:
    kis_recursive_timed_mutex *cpplock;
};

class local_shared_unlocker {
public:
    local_shared_unlocker(kis_recursive_timed_mutex *in) : 
        cpplock{*in} { }

    local_shared_unlocker(kis_recursive_timed_mutex& in) :
        cpplock{in} { }

    ~local_shared_unlocker() {
        cpplock.unlock_shared();
    }

protected:
    kis_recursive_timed_mutex& cpplock;
};

#endif

