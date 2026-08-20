#pragma once

#include "sanguinius/chronicle.hpp"

#include <mutex>
#include <stdexcept>
#include <vector>

namespace sanguinius::test {

class FakeChronicleRepository final : public ChronicleRepository {
public:
  ProposalResult
  create_or_get_proposal(const CreateProposalRequest &request) override {
    const std::scoped_lock lock{mutex_};
    proposals_.push_back(request);
    ChronicleEntry entry{
        .entry_id = request.entry_id,
        .type = request.type,
        .title = request.title,
        .body = request.body,
        .visibility = request.visibility,
        .status = ChronicleEntryStatus::proposed,
        .created_by_user_id = request.proposer_user_id,
        .source_author_user_id = request.source.author.user_id,
        .source_guild_id = request.source.reference.guild_id,
        .source_channel_id = request.source.reference.channel_id,
        .source_message_id = request.source.reference.message_id,
        .source_text = request.source.content,
        .source_text_truncated = request.source.content_truncated,
        .occurred_at_ms = request.source.occurred_at_ms,
        .created_at_ms = request.now_ms};
    for (const auto &participant : request.source.mentioned_users) {
      if (!participant.is_bot && participant.user_id.is_set())
        entry.participants.push_back(participant.user_id);
    }
    if (request.owner_test)
      entry.tags.push_back("owner-test");
    for (const auto &attachment : request.source.attachments) {
      entry.attachments.push_back(
          ChronicleAttachment{.attachment_id = attachment.attachment_id,
                              .filename = attachment.filename,
                              .content_type = attachment.content_type,
                              .byte_size = attachment.byte_size,
                              .width = attachment.width,
                              .height = attachment.height,
                              .ephemeral = attachment.ephemeral,
                              .spoiler = attachment.spoiler});
    }
    return {.code = ChronicleResultCode::created,
            .entry = std::move(entry),
            .actions = request.actions};
  }

  ChronicleMutationResult
  edit_proposal(const EditProposalRequest &request) override {
    const std::scoped_lock lock{mutex_};
    edits_.push_back(request);
    return {.code = ChronicleResultCode::updated};
  }
  ChronicleMutationResult
  submit_proposal(const SubmitProposalRequest &request) override {
    const std::scoped_lock lock{mutex_};
    submissions_.push_back(request);
    return submit_result_;
  }
  ChronicleMutationResult
  apply_approval(const ApplyApprovalRequest &request) override {
    const std::scoped_lock lock{mutex_};
    approvals_.push_back(request);
    return {.code = ChronicleResultCode::updated};
  }
  ChronicleMutationResult
  confirm_memory(const ConfirmMemoryRequest &request) override {
    const std::scoped_lock lock{mutex_};
    if (fail_next_confirmation_) {
      fail_next_confirmation_ = false;
      throw std::runtime_error{"injected Chronicle confirmation failure"};
    }
    confirmations_.push_back(request);
    return {.code = ChronicleResultCode::created,
            .wake_scheduler = request.expiry_job_id.has_value()};
  }
  ChronicleMutationResult
  retract_memory(const RetractItemRequest &request) override {
    const std::scoped_lock lock{mutex_};
    retractions_.push_back(request);
    return {.code = ChronicleResultCode::updated};
  }
  ChronicleMutationResult
  retract_entry(const RetractItemRequest &request) override {
    const std::scoped_lock lock{mutex_};
    retractions_.push_back(request);
    return {.code = ChronicleResultCode::updated};
  }
  ChronicleMutationResult expire_memory(const ClaimedScheduledJob &job,
                                        std::string, std::int64_t) override {
    return {.code = job.job_type == memory_expiry_job_type &&
                            std::holds_alternative<MemoryExpiryJobPayload>(
                                job.payload)
                        ? ChronicleResultCode::updated
                        : ChronicleResultCode::invalid_state};
  }
  RecallResults recall(const DiscordSnowflake &, std::string_view, std::int64_t,
                       std::size_t) override {
    return recall_results;
  }
  std::vector<ChronicleEntry> timeline(std::optional<std::int64_t>,
                                       std::int64_t, std::size_t) override {
    return timeline_results;
  }
  std::vector<ManageableChronicleItem>
  manageable(const DiscordSnowflake &, const DiscordSnowflake &,
             std::string_view, std::int64_t, std::size_t) override {
    return manageable_results;
  }

  [[nodiscard]] std::size_t proposal_count() const {
    const std::scoped_lock lock{mutex_};
    return proposals_.size();
  }
  [[nodiscard]] std::optional<CreateProposalRequest> latest_proposal() const {
    const std::scoped_lock lock{mutex_};
    return proposals_.empty()
               ? std::nullopt
               : std::optional<CreateProposalRequest>{proposals_.back()};
  }
  [[nodiscard]] std::size_t confirmation_count() const {
    const std::scoped_lock lock{mutex_};
    return confirmations_.size();
  }
  [[nodiscard]] std::size_t edit_count() const {
    const std::scoped_lock lock{mutex_};
    return edits_.size();
  }
  [[nodiscard]] std::size_t submission_count() const {
    const std::scoped_lock lock{mutex_};
    return submissions_.size();
  }
  [[nodiscard]] std::size_t approval_count() const {
    const std::scoped_lock lock{mutex_};
    return approvals_.size();
  }
  [[nodiscard]] std::size_t retraction_count() const {
    const std::scoped_lock lock{mutex_};
    return retractions_.size();
  }
  void set_submit_result(ChronicleMutationResult result) {
    const std::scoped_lock lock{mutex_};
    submit_result_ = std::move(result);
  }
  void fail_next_confirmation() {
    const std::scoped_lock lock{mutex_};
    fail_next_confirmation_ = true;
  }

  RecallResults recall_results;
  std::vector<ChronicleEntry> timeline_results;
  std::vector<ManageableChronicleItem> manageable_results;

private:
  mutable std::mutex mutex_;
  std::vector<CreateProposalRequest> proposals_;
  std::vector<EditProposalRequest> edits_;
  std::vector<SubmitProposalRequest> submissions_;
  std::vector<ApplyApprovalRequest> approvals_;
  std::vector<ConfirmMemoryRequest> confirmations_;
  std::vector<RetractItemRequest> retractions_;
  ChronicleMutationResult submit_result_{
      .code = ChronicleResultCode::invalid_token};
  bool fail_next_confirmation_{};
};

} // namespace sanguinius::test
