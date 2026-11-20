#include "HaivenAnalyticsCore.h"

#include <string>

using namespace Haiven;

#define CHECK(Condition) do { if (!(Condition)) return __LINE__; } while (false)

static RestTypes::AnalyticsEvent Event(std::string Type, std::string Properties = "{}")
{
	RestTypes::AnalyticsEvent Result;
	Result.Type = std::move(Type);
	Result.Properties = std::move(Properties);
	return Result;
}

static RestTypes::AnalyticsContext Context()
{
	RestTypes::AnalyticsContext Result;
	Result.Environment = "development";
	Result.IntegrationId = "game-1";
	Result.Platform = "unreal";
	RestTypes::AnalyticsSDK Sdk;
	Sdk.Name = "haiven-unreal";
	Sdk.Version = "0.0.0";
	Result.Sdk = Sdk;
	return Result;
}

int main()
{
	FAnalyticsBatcherOptions InvalidOptions;
	InvalidOptions.BatchSize = 101;
	FAnalyticsBatcherCore Invalid(
		Context(), InvalidOptions, "session-1", [] { return "event"; }, [] { return "now"; });
	CHECK(!Invalid.IsValid());
	CHECK(!Invalid.GetConfigurationError().empty());
	std::optional<FAnalyticsDiagnostic> InvalidDiagnostic;
	CHECK(!Invalid.TryEnqueue(Event("sdk.health"), InvalidDiagnostic));
	CHECK(InvalidDiagnostic && InvalidDiagnostic->Kind == EAnalyticsDiagnosticKind::EventRejected);
	CHECK(!Invalid.BeginBatch());

	FAnalyticsBatcherOptions Options;
	Options.BatchSize = 2;
	Options.MaxBufferedEvents = 3;
	Options.MaxRetries = 2;
	Options.InitialRetryDelayMilliseconds = 10;
	Options.MaxRetryDelayMilliseconds = 15;
	int Id = 0;
	FAnalyticsBatcherCore Batcher(
		Context(),
		Options,
		"session-1",
		[&Id] { return "event-" + std::to_string(++Id); },
		[] { return "2026-10-14T12:00:00.000Z"; });
	CHECK(Batcher.IsValid() && Batcher.IsIdle());

	std::optional<FAnalyticsDiagnostic> Rejected;
	CHECK(!Batcher.TryEnqueue(Event("unknown"), Rejected));
	CHECK(Rejected && Rejected->Reason.find("event type") != std::string::npos);
	CHECK(!Batcher.TryEnqueue(Event("sdk.health", "not-json"), Rejected));
	CHECK(Rejected && Rejected->Reason.find("malformed JSON") != std::string::npos);
	CHECK(!Batcher.TryEnqueue(Event("sdk.health", "[]"), Rejected));
	CHECK(Rejected && Rejected->Reason.find("JSON object") != std::string::npos);
	CHECK(!Batcher.TryEnqueue(Event("sdk.health", R"({"nested":{"value":1}})"), Rejected));
	CHECK(Rejected && Rejected->Reason.find("flat scalar") != std::string::npos);
	std::string TooManyProperties = "{";
	for (int Index = 0; Index < 33; ++Index)
	{
		if (Index > 0) TooManyProperties += ",";
		TooManyProperties += "\"p" + std::to_string(Index) + "\":" + std::to_string(Index);
	}
	TooManyProperties += "}";
	CHECK(!Batcher.TryEnqueue(Event("sdk.health", TooManyProperties), Rejected));
	CHECK(Rejected && Rejected->Reason.find("at most 32") != std::string::npos);
	CHECK(!Batcher.TryEnqueue(Event("sdk.error"), Rejected));
	CHECK(Rejected && Rejected->Reason.find("errorType and operation") != std::string::npos);
	CHECK(!Batcher.TryEnqueue(Event("sdk.error", R"({"errorType":"timeout"})"), Rejected));
	CHECK(Rejected && Rejected->Reason.find("errorType and operation") != std::string::npos);
	CHECK(!Batcher.TryEnqueue(Event("sdk.error", R"({"errorType":"","operation":"chat.send"})"), Rejected));
	CHECK(!Batcher.TryEnqueue(Event("sdk.error", R"({"errorType":1,"operation":"chat.send"})"), Rejected));
	CHECK(Batcher.IsIdle());

	{
		int ErrorId = 0;
		FAnalyticsBatcherCore ErrorBatcher(
			Context(),
			Options,
			"session-error",
			[&ErrorId] { return "error-event-" + std::to_string(++ErrorId); },
			[] { return "2026-10-14T12:00:00.000Z"; });
		CHECK(ErrorBatcher.TryEnqueue(Event(
			"sdk.error", R"({"errorType":"timeout","operation":"chat.send"})")));
		const auto ErrorBatch = ErrorBatcher.BeginBatch();
		CHECK(ErrorBatch);
		CHECK(ErrorBatch->Body.find(R"("type":"sdk.error")") != std::string::npos);
		CHECK(ErrorBatch->Body.find(R"("errorType":"timeout")") != std::string::npos);
		CHECK(ErrorBatch->Body.find(R"("operation":"chat.send")") != std::string::npos);
		CHECK(ErrorBatcher.HandleReply(202, R"({"accepted":1,"duplicate":0})").Action
			== FAnalyticsDispatchResult::EAction::Delivered);
	}

	auto Mutable = Event("sdk.health", R"({"status":"ok","sampled":true,"load":1.5,"note":null})");
	CHECK(Batcher.TryEnqueue(Mutable));
	Mutable.Type = "session.end";
	Mutable.Properties = R"({"status":"changed"})";
	CHECK(Batcher.TryEnqueue(Event("game.launch", R"({"source":"menu"})")));
	CHECK(Batcher.TryEnqueue(Event("playtime", R"({"seconds":30})")));
	std::optional<FAnalyticsDiagnostic> Full;
	CHECK(!Batcher.TryEnqueue(Event("session.end"), Full));
	CHECK(Full && Full->Kind == EAnalyticsDiagnosticKind::QueueFull);
	CHECK(Batcher.BufferedEventCount() == 3);
	CHECK(Batcher.DroppedEventCount() == 1);

	const auto First = Batcher.BeginBatch();
	CHECK(First);
	CHECK(First->Method == "POST");
	CHECK(First->Path == "/sdk/analytics/events");
	CHECK(First->Body.find(R"("analyticsSessionId":"session-1")") != std::string::npos);
	CHECK(First->Body.find(R"("eventId":"event-1")") != std::string::npos);
	CHECK(First->Body.find(R"("occurredAt":"2026-10-14T12:00:00.000Z")") != std::string::npos);
	CHECK(First->Body.find(R"("type":"sdk.health")") != std::string::npos);
	CHECK(First->Body.find(R"("status":"ok")") != std::string::npos);
	CHECK(First->Body.find("changed") == std::string::npos);
	CHECK(Batcher.InFlightEventCount() == 2 && Batcher.QueuedEventCount() == 1);

	const auto Retry1 = Batcher.HandleReply(0, "", "connection reset");
	CHECK(Retry1.Action == FAnalyticsDispatchResult::EAction::Retry);
	CHECK(Retry1.RetryAfterMilliseconds == 10);
	CHECK(Retry1.Diagnostics.size() == 1);
	CHECK(Retry1.Diagnostics[0].Kind == EAnalyticsDiagnosticKind::RetryScheduled);
	CHECK(Retry1.Diagnostics[0].Attempt == 1);
	CHECK(Retry1.Diagnostics[0].EventIds[0] == "event-1");
	const auto Retained1 = Batcher.BeginBatch();
	CHECK(Retained1 && Retained1->Body == First->Body);

	const auto Retry2 = Batcher.HandleReply(503, "");
	CHECK(Retry2.Action == FAnalyticsDispatchResult::EAction::Retry);
	CHECK(Retry2.RetryAfterMilliseconds == 15);
	CHECK(Retry2.Diagnostics[0].Attempt == 2);
	const auto Retained2 = Batcher.BeginBatch();
	CHECK(Retained2 && Retained2->Body == First->Body);

	const auto Delivered = Batcher.HandleReply(
		202,
		R"({"accepted":1,"duplicate":0,"rejected":[{"index":1,"reason":"invalid_properties"}]})");
	CHECK(Delivered.Action == FAnalyticsDispatchResult::EAction::Delivered);
	CHECK(Delivered.Diagnostics.size() == 1);
	CHECK(Delivered.Diagnostics[0].Kind == EAnalyticsDiagnosticKind::EventRejected);
	CHECK(Delivered.Diagnostics[0].EventIds[0] == "event-2");
	CHECK(Batcher.RetryCount() == 2);
	CHECK(Batcher.BufferedEventCount() == 1);

	const auto Last = Batcher.BeginBatch();
	CHECK(Last && Last->Body.find(R"("eventId":"event-3")") != std::string::npos);
	const auto EmptySuccess = Batcher.HandleReply(202, "");
	CHECK(EmptySuccess.Action == FAnalyticsDispatchResult::EAction::Retry);
	CHECK(EmptySuccess.Diagnostics[0].Reason.find("empty") != std::string::npos);
	CHECK(Batcher.BeginBatch()->Body == Last->Body);
	CHECK(Batcher.HandleReply(503, "").Action == FAnalyticsDispatchResult::EAction::Retry);
	const auto Exhausted = Batcher.HandleReply(503, "");
	CHECK(Exhausted.Action == FAnalyticsDispatchResult::EAction::Dropped);
	CHECK(Exhausted.Diagnostics[0].Reason.find("retry limit reached") != std::string::npos);
	CHECK(Batcher.DroppedEventCount() == 2);
	CHECK(Batcher.IsIdle());

	const auto StrayReply = Batcher.HandleReply(202, "{}");
	CHECK(StrayReply.Action == FAnalyticsDispatchResult::EAction::Dropped);
	CHECK(StrayReply.Diagnostics[0].Reason.find("no in-flight") != std::string::npos);

	CHECK(Batcher.TryEnqueue(Event("session.end", R"({"reason":"shutdown"})")));
	CHECK(Batcher.BeginBatch());
	const auto Disposal = Batcher.Dispose();
	CHECK(Disposal.size() == 1);
	CHECK(Disposal[0].Kind == EAnalyticsDiagnosticKind::BatchDropped);
	CHECK(Disposal[0].EventIds[0] == "event-5");
	CHECK(Batcher.IsIdle());
	CHECK(Batcher.DroppedEventCount() == 3);
	CHECK(Batcher.Dispose().empty());
	CHECK(!Batcher.TryEnqueue(Event("sdk.health"), Rejected));
	CHECK(Rejected && Rejected->Reason.find("disposed") != std::string::npos);
}
