// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/subresource_filter/content/renderer/subresource_filter_agent.h"

#include <vector>

#include "base/feature_list.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/memory/ref_counted.h"
#include "base/metrics/histogram_macros.h"
#include "base/time/time.h"
#include "components/subresource_filter/content/common/subresource_filter_messages.h"
#include "components/subresource_filter/content/renderer/unverified_ruleset_dealer.h"
#include "components/subresource_filter/content/renderer/web_document_subresource_filter_impl.h"
#include "components/subresource_filter/core/common/document_load_statistics.h"
#include "components/subresource_filter/core/common/document_subresource_filter.h"
#include "components/subresource_filter/core/common/memory_mapped_ruleset.h"
#include "components/subresource_filter/core/common/scoped_timers.h"
#include "components/subresource_filter/core/common/time_measurements.h"
#include "content/public/common/content_features.h"
#include "content/public/common/url_constants.h"
#include "content/public/renderer/render_frame.h"
#include "ipc/ipc_message.h"
#include "third_party/WebKit/public/platform/WebWorkerFetchContext.h"
#include "third_party/WebKit/public/web/WebDataSource.h"
#include "third_party/WebKit/public/web/WebDocument.h"
#include "third_party/WebKit/public/web/WebLocalFrame.h"
#include "url/url_constants.h"

namespace subresource_filter {

SubresourceFilterAgent::SubresourceFilterAgent(
    content::RenderFrame* render_frame,
    UnverifiedRulesetDealer* ruleset_dealer)
    : content::RenderFrameObserver(render_frame),
      content::RenderFrameObserverTracker<SubresourceFilterAgent>(render_frame),
      ruleset_dealer_(ruleset_dealer) {
  DCHECK(ruleset_dealer);
}

SubresourceFilterAgent::~SubresourceFilterAgent() = default;

GURL SubresourceFilterAgent::GetDocumentURL() {
  return render_frame()->GetWebFrame()->GetDocument().Url();
}

void SubresourceFilterAgent::SetSubresourceFilterForCommittedLoad(
    std::unique_ptr<blink::WebDocumentSubresourceFilter> filter) {
  blink::WebLocalFrame* web_frame = render_frame()->GetWebFrame();
  web_frame->DataSource()->SetSubresourceFilter(filter.release());
}

void SubresourceFilterAgent::
    SignalFirstSubresourceDisallowedForCommittedLoad() {
  render_frame()->Send(new SubresourceFilterHostMsg_DidDisallowFirstSubresource(
      render_frame()->GetRoutingID()));
}

void SubresourceFilterAgent::SendDocumentLoadStatistics(
    const DocumentLoadStatistics& statistics) {
  render_frame()->Send(new SubresourceFilterHostMsg_DocumentLoadStatistics(
      render_frame()->GetRoutingID(), statistics));
}

// static
ActivationState SubresourceFilterAgent::GetParentActivationState(
    content::RenderFrame* render_frame) {
  blink::WebFrame* parent =
      render_frame ? render_frame->GetWebFrame()->Parent() : nullptr;
  if (parent && parent->IsWebLocalFrame()) {
    auto* agent = SubresourceFilterAgent::Get(
        content::RenderFrame::FromWebFrame(parent->ToWebLocalFrame()));
    if (agent && agent->filter_for_last_committed_load_)
      return agent->filter_for_last_committed_load_->activation_state();
  }
  return ActivationState(ActivationLevel::DISABLED);
}

void SubresourceFilterAgent::OnActivateForNextCommittedLoad(
    ActivationState activation_state) {
  activation_state_for_next_commit_ = activation_state;
}

void SubresourceFilterAgent::RecordHistogramsOnLoadCommitted() {
  // Note: ActivationLevel used to be called ActivationState, the legacy name is
  // kept for the histogram.
  ActivationLevel activation_level =
      activation_state_for_next_commit_.activation_level;
  UMA_HISTOGRAM_ENUMERATION("SubresourceFilter.DocumentLoad.ActivationState",
                            static_cast<int>(activation_level),
                            static_cast<int>(ActivationLevel::LAST) + 1);

  if (activation_level != ActivationLevel::DISABLED) {
    UMA_HISTOGRAM_BOOLEAN("SubresourceFilter.DocumentLoad.RulesetIsAvailable",
                          ruleset_dealer_->IsRulesetFileAvailable());
  }
}

void SubresourceFilterAgent::RecordHistogramsOnLoadFinished() {
  DCHECK(filter_for_last_committed_load_);
  const auto& statistics =
      filter_for_last_committed_load_->filter().statistics();

  UMA_HISTOGRAM_COUNTS_1000(
      "SubresourceFilter.DocumentLoad.NumSubresourceLoads.Total",
      statistics.num_loads_total);
  UMA_HISTOGRAM_COUNTS_1000(
      "SubresourceFilter.DocumentLoad.NumSubresourceLoads.Evaluated",
      statistics.num_loads_evaluated);
  UMA_HISTOGRAM_COUNTS_1000(
      "SubresourceFilter.DocumentLoad.NumSubresourceLoads.MatchedRules",
      statistics.num_loads_matching_rules);
  UMA_HISTOGRAM_COUNTS_1000(
      "SubresourceFilter.DocumentLoad.NumSubresourceLoads.Disallowed",
      statistics.num_loads_disallowed);

  // If ThreadTicks is not supported or performance measuring is switched off,
  // then no time measurements have been collected.
  if (ScopedThreadTimers::IsSupported() &&
      filter_for_last_committed_load_->filter()
          .activation_state()
          .measure_performance) {
    UMA_HISTOGRAM_CUSTOM_MICRO_TIMES(
        "SubresourceFilter.DocumentLoad.SubresourceEvaluation."
        "TotalWallDuration",
        statistics.evaluation_total_wall_duration,
        base::TimeDelta::FromMicroseconds(1), base::TimeDelta::FromSeconds(10),
        50);
    UMA_HISTOGRAM_CUSTOM_MICRO_TIMES(
        "SubresourceFilter.DocumentLoad.SubresourceEvaluation.TotalCPUDuration",
        statistics.evaluation_total_cpu_duration,
        base::TimeDelta::FromMicroseconds(1), base::TimeDelta::FromSeconds(10),
        50);
  } else {
    DCHECK(statistics.evaluation_total_wall_duration.is_zero());
    DCHECK(statistics.evaluation_total_cpu_duration.is_zero());
  }

  SendDocumentLoadStatistics(statistics);
}

void SubresourceFilterAgent::ResetActivatonStateForNextCommit() {
  activation_state_for_next_commit_ =
      ActivationState(ActivationLevel::DISABLED);
}

void SubresourceFilterAgent::OnDestruct() {
  delete this;
}

void SubresourceFilterAgent::DidCommitProvisionalLoad(
    bool is_new_navigation,
    bool is_same_document_navigation) {
  if (is_same_document_navigation)
    return;

  filter_for_last_committed_load_.reset();

  // TODO(csharrison): Use WebURL and WebSecurityOrigin for efficiency here,
  // which require changes to the unit tests.
  const GURL& url = GetDocumentURL();

  bool use_parent_activation = ShouldUseParentActivation(url);
  if (use_parent_activation) {
    activation_state_for_next_commit_ =
        GetParentActivationState(render_frame());
  }

  if (url.SchemeIsHTTPOrHTTPS() || url.SchemeIsFile() ||
      use_parent_activation) {
    RecordHistogramsOnLoadCommitted();
    if (activation_state_for_next_commit_.activation_level !=
            ActivationLevel::DISABLED &&
        ruleset_dealer_->IsRulesetFileAvailable()) {
      base::OnceClosure first_disallowed_load_callback(
          base::BindOnce(&SubresourceFilterAgent::
                             SignalFirstSubresourceDisallowedForCommittedLoad,
                         AsWeakPtr()));

      auto ruleset = ruleset_dealer_->GetRuleset();
      // TODO(csharrison): Replace with DCHECK when crbug.com/734102 is
      // resolved.
      CHECK(ruleset);
      CHECK(ruleset->data());
      auto filter = base::MakeUnique<WebDocumentSubresourceFilterImpl>(
          url::Origin(url), activation_state_for_next_commit_,
          std::move(ruleset), std::move(first_disallowed_load_callback));

      filter_for_last_committed_load_ = filter->AsWeakPtr();
      SetSubresourceFilterForCommittedLoad(std::move(filter));
    }
  }

  ResetActivatonStateForNextCommit();
}

void SubresourceFilterAgent::DidFailProvisionalLoad(
    const blink::WebURLError& error) {
  // TODO(engedy): Add a test with `frame-ancestor` violation to exercise this.
  ResetActivatonStateForNextCommit();
}

void SubresourceFilterAgent::DidFinishLoad() {
  if (!filter_for_last_committed_load_)
    return;
  RecordHistogramsOnLoadFinished();
}

bool SubresourceFilterAgent::OnMessageReceived(const IPC::Message& message) {
  bool handled = true;
  IPC_BEGIN_MESSAGE_MAP(SubresourceFilterAgent, message)
    IPC_MESSAGE_HANDLER(SubresourceFilterMsg_ActivateForNextCommittedLoad,
                        OnActivateForNextCommittedLoad)
    IPC_MESSAGE_UNHANDLED(handled = false)
  IPC_END_MESSAGE_MAP()
  return handled;
}

void SubresourceFilterAgent::WillCreateWorkerFetchContext(
    blink::WebWorkerFetchContext* worker_fetch_context) {
  DCHECK(base::FeatureList::IsEnabled(features::kOffMainThreadFetch));
  if (!filter_for_last_committed_load_)
    return;
  if (!ruleset_dealer_->IsRulesetFileAvailable())
    return;
  base::File ruleset_file = ruleset_dealer_->DuplicateRulesetFile();
  if (!ruleset_file.IsValid())
    return;
  worker_fetch_context->SetSubresourceFilterBuilder(
      base::MakeUnique<WebDocumentSubresourceFilterImpl::BuilderImpl>(
          url::Origin(GetDocumentURL()),
          filter_for_last_committed_load_->filter().activation_state(),
          std::move(ruleset_file),
          base::BindOnce(&SubresourceFilterAgent::
                             SignalFirstSubresourceDisallowedForCommittedLoad,
                         AsWeakPtr())));
}

bool SubresourceFilterAgent::ShouldUseParentActivation(const GURL& url) const {
  // TODO(csharrison): It is not always true that a data URL can use its
  // parent's activation in OOPIF mode, where the resulting data frame will
  // be same-process to its initiator. See crbug.com/739777 for more
  // information.
  return render_frame() && !render_frame()->IsMainFrame() &&
         (url.SchemeIs(url::kDataScheme) || url == url::kAboutBlankURL ||
          url == content::kAboutSrcDocURL);
}

}  // namespace subresource_filter
