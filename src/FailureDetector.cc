/* Copyright (c) 2011-2012 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <errno.h>
#include <fcntl.h>

#include "Common.h"
#include "CycleCounter.h"
#include "Cycles.h"
#include "Fence.h"
#include "PingClient.h"
#include "CoordinatorClient.h"
#include "FailureDetector.h"
#include "IpAddress.h"
#include "ShortMacros.h"
#include "WireFormat.h"

namespace RAMCloud {

/**
 * Create a new FailureDetector object. Note that this class depends on the
 * MembershipService running and keeping the global Context::serverList up
 * to date. Without it, we'd not know of new servers to ping.
 *
 * \param context
 *      Overall information about the RAMCloud server.
 * \param[in] ourServerId
 *      The ServerId of this server, as returned by enlistment with the
 *      coordinator. Used only to avoid pinging ourself.
 */
FailureDetector::FailureDetector(Context* context,
                                 const ServerId ourServerId)
    : context(context),
      ourServerId(ourServerId),
      serverTracker(context),
      thread(),
      threadShouldExit(false)
{
}

FailureDetector::~FailureDetector()
{
    halt();
}

/**
 * Start the failure detector thread.
 *
 * This method starts the failure detector thread and returns immediately. Use
 * the halt method to terminate the failure detector thread. A valid context
 * must be initialized before calling start.
 */
void
FailureDetector::start()
{
    thread.construct(detectorThreadEntry, this, context);
}

/**
 * Halt the failure detector thread.
 *
 * This method stops the failure detector thread. Once the function returns
 * the thread will have terminated and cleaned up after itself.
 */
void
FailureDetector::halt()
{
    if (thread) {
        threadShouldExit = true;
        Fence::sfence();
        thread->join();
        threadShouldExit = false;
        thread.destroy();
    }
}

/**
 * Main thread entry point for the failure detector. Spin forever, probing
 * hosts, checkings for responses, and alerting the coordinator of any timeouts.
 *
 * \param detector
 *          FailureDetector passed in from thread creation.
 * \param context
 *          Context object to use for all operations.
 */
void
FailureDetector::detectorThreadEntry(FailureDetector* detector,
                                     Context* context)
{
    LOG(NOTICE, "Failure detector thread started");

    while (1) {
        // Check if we have been requested to exit.
        Fence::lfence();
        if (detector->threadShouldExit)
            break;

        // Drain the list of changes to update our tracker.
        ServerDetails dummy1;
        ServerChangeEvent dummy2;
        while (detector->serverTracker.getChange(dummy1, dummy2)) {
        }

        // Ping a random server
        detector->pingRandomServer();

        // Sleep for the specified interval
        usleep(PROBE_INTERVAL_USECS);
    }
}

/**
 * Choose a random server from our list and ping it. Only one oustanding
 * ping is issued at a given time. If a timeout occurs we attempt to notify
 * the coordinator.
 */
void
FailureDetector::pingRandomServer()
{
    ServerId pingee = serverTracker.getRandomServerIdWithService(
        WireFormat::PING_SERVICE);
    if (!pingee.isValid() || pingee == ourServerId) {
        // If there isn't anyone to talk to, or the host selected
        // is ourself, then just skip this round and try again
        // on the next ping interval.
        return;
    }

    string locator;
    try {
        locator = context->serverList->getLocator(pingee);
        uint64_t serverListVersion;
        LOG(DEBUG, "Sending ping to server %s (%s)", pingee.toString().c_str(),
            locator.c_str());
        uint64_t start = Cycles::rdtsc();
        PingRpc rpc(context, pingee, ourServerId);
        serverListVersion = rpc.wait(TIMEOUT_USECS *1000);
        if (serverListVersion == ~0LU) {
            // Server appears to have crashed; notify the coordinator.
            LOG(WARNING, "Ping timeout to server id %s (locator \"%s\")",
                pingee.toString().c_str(), locator.c_str());
            CoordinatorClient::hintServerDown(context, pingee);
            return;
        }
        LOG(DEBUG, "Ping succeeded to server %s (%s) in %.1f us",
            pingee.toString().c_str(), locator.c_str(),
            1e06*Cycles::toSeconds(Cycles::rdtsc() - start));
    } catch (const ServerListException &sle) {
        // This isn't an error. It's just a race between this thread and
        // the membership service. It should be quite uncommon, so just
        // bail on this round and ping again next time.
        LOG(NOTICE, "Tried to ping locator \"%s\", but id %s was stale",
            locator.c_str(), pingee.toString().c_str());
    }
}
} // namespace
