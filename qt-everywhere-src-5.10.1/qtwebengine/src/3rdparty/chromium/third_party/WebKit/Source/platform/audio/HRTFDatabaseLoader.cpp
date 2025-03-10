/*
 * Copyright (C) 2010 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Computer, Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "platform/audio/HRTFDatabaseLoader.h"

#include "platform/CrossThreadFunctional.h"
#include "platform/WaitableEvent.h"
#include "platform/WebTaskRunner.h"
#include "platform/wtf/PtrUtil.h"
#include "public/platform/Platform.h"
#include "public/platform/WebTraceLocation.h"

namespace blink {

using LoaderMap = HashMap<double, HRTFDatabaseLoader*>;

// getLoaderMap() returns the static hash map that contains the mapping between
// the sample rate and the corresponding HRTF database.
static LoaderMap& GetLoaderMap() {
  DEFINE_STATIC_LOCAL(LoaderMap*, map, (new LoaderMap));
  return *map;
}

PassRefPtr<HRTFDatabaseLoader>
HRTFDatabaseLoader::CreateAndLoadAsynchronouslyIfNecessary(float sample_rate) {
  DCHECK(IsMainThread());

  RefPtr<HRTFDatabaseLoader> loader = GetLoaderMap().at(sample_rate);
  if (loader) {
    DCHECK_EQ(sample_rate, loader->DatabaseSampleRate());
    return loader;
  }

  loader = AdoptRef(new HRTFDatabaseLoader(sample_rate));
  GetLoaderMap().insert(sample_rate, loader.Get());
  loader->LoadAsynchronously();
  return loader;
}

HRTFDatabaseLoader::HRTFDatabaseLoader(float sample_rate)
    : database_sample_rate_(sample_rate) {
  DCHECK(IsMainThread());
}

HRTFDatabaseLoader::~HRTFDatabaseLoader() {
  DCHECK(IsMainThread());
  DCHECK(!thread_);
  GetLoaderMap().erase(database_sample_rate_);
}

void HRTFDatabaseLoader::LoadTask() {
  DCHECK(!IsMainThread());
  DCHECK(!hrtf_database_);

  // Protect access to m_hrtfDatabase, which can be accessed from the audio
  // thread.
  MutexLocker locker(lock_);
  // Load the default HRTF database.
  hrtf_database_ = HRTFDatabase::Create(database_sample_rate_);
}

void HRTFDatabaseLoader::LoadAsynchronously() {
  DCHECK(IsMainThread());

  // m_hrtfDatabase and m_thread should both be unset because this should be a
  // new HRTFDatabaseLoader object that was just created by
  // createAndLoadAsynchronouslyIfNecessary and because we haven't started
  // loadTask yet for this object.
  DCHECK(!hrtf_database_);
  DCHECK(!thread_);

  // Start the asynchronous database loading process.
  thread_ = Platform::Current()->CreateThread("HRTF database loader");
  // TODO(alexclarke): Should this be posted as a loading task?
  thread_->GetWebTaskRunner()->PostTask(
      BLINK_FROM_HERE, CrossThreadBind(&HRTFDatabaseLoader::LoadTask,
                                       CrossThreadUnretained(this)));
}

HRTFDatabase* HRTFDatabaseLoader::Database() {
  DCHECK(!IsMainThread());

  // Seeing that this is only called from the audio thread, we can't block.
  // It's ok to return nullptr if we can't get the lock.
  MutexTryLocker try_locker(lock_);

  if (!try_locker.Locked())
    return nullptr;

  return hrtf_database_.get();
}

// This cleanup task is needed just to make sure that the loader thread finishes
// the load task and thus the loader thread doesn't touch m_thread any more.
void HRTFDatabaseLoader::CleanupTask(WaitableEvent* sync) {
  sync->Signal();
}

void HRTFDatabaseLoader::WaitForLoaderThreadCompletion() {
  if (!thread_)
    return;

  WaitableEvent sync;
  // TODO(alexclarke): Should this be posted as a loading task?
  thread_->GetWebTaskRunner()->PostTask(
      BLINK_FROM_HERE, CrossThreadBind(&HRTFDatabaseLoader::CleanupTask,
                                       CrossThreadUnretained(this),
                                       CrossThreadUnretained(&sync)));
  sync.Wait();
  thread_.reset();
}

}  // namespace blink
