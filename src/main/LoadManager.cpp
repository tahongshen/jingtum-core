//------------------------------------------------------------------------------
/*
    This file is part of skywelld: https://github.com/skywell/skywelld
    Copyright (c) 2012, 2013 Skywell Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <BeastConfig.h>
#include <beast/threads/Thread.h>
#include <mutex>
#include <thread>
#include <main/LoadManager.h>
#include <main/Application.h>
#include <common/misc/NetworkOPs.h>
#include <common/base/UptimeTimer.h>
#include <common/core/JobQueue.h>
#include <common/core/LoadFeeTrack.h>
#include <common/json/to_string.h>


namespace skywell {

class LoadManagerImp
    : public LoadManager
    , public beast::Thread
{
public:
    //--------------------------------------------------------------------------

    beast::Journal m_journal;
    using LockType = std::mutex;
    typedef std::lock_guard <LockType> ScopedLockType;
    LockType mLock;

    bool mArmed;

    int mDeadLock;              // Detect server deadlocks
    //--------------------------------------------------------------------------

    LoadManagerImp (Stoppable& parent, beast::Journal journal)
        : LoadManager (parent)
        , Thread ("loadmgr")
        , m_journal (journal)
        , mArmed (false)
        , mDeadLock (0)
    {
        UptimeTimer::getInstance ().beginManualUpdates ();
    }

    ~LoadManagerImp ()
    {
        UptimeTimer::getInstance ().endManualUpdates ();

        stopThread ();
    }

    //--------------------------------------------------------------------------
    //
    // Stoppable
    //
    //--------------------------------------------------------------------------

    void onPrepare ()
    {
    }

    void onStart ()
    {
        m_journal.debug << "Starting";
        startThread ();
    }

    void onStop ()
    {
        if (isThreadRunning ())
        {
            m_journal.debug << "Stopping";

            stopThreadAsync ();
        }
        else
        {
            stopped ();
        }
    }

    //--------------------------------------------------------------------------

    void resetDeadlockDetector ()
    {
        ScopedLockType sl (mLock);

        mDeadLock = UptimeTimer::getInstance ().getElapsedSeconds ();
    }

    void activateDeadlockDetector ()
    {
        mArmed = true;
    }

    void logDeadlock (int dlTime)
    {
        m_journal.warning << "Server stalled for " << dlTime << " seconds.";
    }

    //  NOTE Where's the thread object? It's not a data member...
    //
    void run ()
    {
        using clock_type = std::chrono::steady_clock;

        // Initialize the clock to the current time.
        auto t = clock_type::now();

        while (! threadShouldExit ())
        {
            {
                //  NOTE What is this lock protecting?
                ScopedLockType sl (mLock);

                //  NOTE I think this is to reduce calls to the operating system
                //             for retrieving the current time.
                //
                //        TODO Instead of incrementing can't we just retrieve the current
                //             time again?
                //
                // Manually update the timer.
                UptimeTimer::getInstance ().incrementElapsedTime ();

                // Measure the amount of time we have been deadlocked, in seconds.
                //
                //  NOTE mDeadLock is a canary for detecting the condition.
                int const timeSpentDeadlocked = UptimeTimer::getInstance ().getElapsedSeconds () - mDeadLock;

                //  NOTE I think that "armed" refers to the deadlock detector
                //
                int const reportingIntervalSeconds = 10;
                if (mArmed && (timeSpentDeadlocked >= reportingIntervalSeconds))
                {
                    // Report the deadlocked condition every 10 seconds
                    if ((timeSpentDeadlocked % reportingIntervalSeconds) == 0)
                    {
                        logDeadlock (timeSpentDeadlocked);
                    }

                    // If we go over 500 seconds spent deadlocked, it means that the
                    // deadlock resolution code has failed, which qualifies as undefined
                    // behavior.
                    //
                    assert (timeSpentDeadlocked < 500);
                }
            }

            bool change;

            //  TODO Eliminate the dependence on the Application object.
            //             Choices include constructing with the job queue / feetracker.
            //             Another option is using an observer pattern to invert the dependency.
            if (getApp().getJobQueue ().isOverloaded ())
            {
                if (m_journal.info)
                    m_journal.info << getApp().getJobQueue ().getJson (0);

                change = getApp().getFeeTrack ().raiseLocalFee ();
            }
            else
            {
                change = getApp().getFeeTrack ().lowerLocalFee ();
            }

            if (change)
            {
                //  TODO replace this with a Listener / observer and subscribe in NetworkOPs or Application
                getApp().getOPs ().reportFeeChange ();
            }

            t += std::chrono::seconds (1);
            auto const duration = t - clock_type::now();

            if ((duration < std::chrono::seconds (0)) || (duration > std::chrono::seconds (1)))
            {
                m_journal.warning << "time jump";

                t = clock_type::now();
            }
            else
            {
                std::this_thread::sleep_for (duration);
            }
        }

        stopped ();
    }
};

//------------------------------------------------------------------------------

LoadManager::LoadManager (Stoppable& parent)
    : Stoppable ("LoadManager", parent)
{
}

//------------------------------------------------------------------------------

std::unique_ptr<LoadManager>
make_LoadManager (beast::Stoppable& parent, beast::Journal journal)
{
    return std::make_unique <LoadManagerImp> (parent, journal);
}

} // skywell
