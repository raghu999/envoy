#include "common/tracing/zipkin/zipkin_tracer_impl.h"

#include "common/common/enum_to_int.h"
#include "common/http/headers.h"
#include "common/http/message_impl.h"
#include "common/http/utility.h"
#include "common/tracing/http_tracer_impl.h"
#include "common/tracing/zipkin/zipkin_core_constants.h"

#include "fmt/format.h"

namespace Envoy {
namespace Zipkin {

ZipkinSpan::ZipkinSpan(Zipkin::Span& span, Zipkin::Tracer& tracer) : span_(span), tracer_(tracer) {}

void ZipkinSpan::finishSpan(Tracing::SpanFinalizer& finalizer) {
  finalizer.finalize(*this);
  span_.finish();
}

void ZipkinSpan::setOperation(const std::string& operation) { span_.setName(operation); }

void ZipkinSpan::setTag(const std::string& name, const std::string& value) {
  span_.setTag(name, value);
}

void ZipkinSpan::injectContext(Http::HeaderMap& request_headers) {
  // Set the trace-id and span-id headers properly, based on the newly-created span structure.
  request_headers.insertXB3TraceId().value(span_.traceIdAsHexString());
  request_headers.insertXB3SpanId().value(span_.idAsHexString());

  // Set the parent-span header properly, based on the newly-created span structure.
  if (span_.isSetParentId()) {
    request_headers.insertXB3ParentSpanId().value(span_.parentIdAsHexString());
  }

  // Set the sampled header.
  request_headers.insertXB3Sampled().value().setReference(ZipkinCoreConstants::get().ALWAYS_SAMPLE);

  // Set the ot-span-context header with the new context.
  SpanContext context(span_);
  request_headers.insertOtSpanContext().value(context.serializeToString());
}

Tracing::SpanPtr ZipkinSpan::spawnChild(const Tracing::Config& config, const std::string& name,
                                        SystemTime start_time) {
  SpanContext context(span_);
  return Tracing::SpanPtr{
      new ZipkinSpan(*tracer_.startSpan(config, name, start_time, context), tracer_)};
}

Driver::TlsTracer::TlsTracer(TracerPtr&& tracer, Driver& driver)
    : tracer_(std::move(tracer)), driver_(driver) {}

Driver::Driver(const Json::Object& config, Upstream::ClusterManager& cluster_manager,
               Stats::Store& stats, ThreadLocal::SlotAllocator& tls, Runtime::Loader& runtime,
               const LocalInfo::LocalInfo& local_info, Runtime::RandomGenerator& random_generator)
    : cm_(cluster_manager), tracer_stats_{ZIPKIN_TRACER_STATS(
                                POOL_COUNTER_PREFIX(stats, "tracing.zipkin."))},
      tls_(tls.allocateSlot()), runtime_(runtime), local_info_(local_info) {

  Upstream::ThreadLocalCluster* cluster = cm_.get(config.getString("collector_cluster"));
  if (!cluster) {
    throw EnvoyException(fmt::format("{} collector cluster is not defined on cluster manager level",
                                     config.getString("collector_cluster")));
  }
  cluster_ = cluster->info();

  const std::string collector_endpoint =
      config.getString("collector_endpoint", ZipkinCoreConstants::get().DEFAULT_COLLECTOR_ENDPOINT);

  tls_->set([this, collector_endpoint, &random_generator](
                Event::Dispatcher& dispatcher) -> ThreadLocal::ThreadLocalObjectSharedPtr {
    TracerPtr tracer(
        new Tracer(local_info_.clusterName(), local_info_.address(), random_generator));
    tracer->setReporter(
        ReporterImpl::NewInstance(std::ref(*this), std::ref(dispatcher), collector_endpoint));
    return ThreadLocal::ThreadLocalObjectSharedPtr{new TlsTracer(std::move(tracer), *this)};
  });
}

Tracing::SpanPtr Driver::startSpan(const Tracing::Config& config, Http::HeaderMap& request_headers,
                                   const std::string&, SystemTime start_time) {
  Tracer& tracer = *tls_->getTyped<TlsTracer>().tracer_;
  SpanPtr new_zipkin_span;

  if (request_headers.OtSpanContext()) {
    // Get the open-tracing span context.
    // This header contains a span's parent-child relationships set by the downstream Envoy.
    // The context built from this header allows the Zipkin tracer to
    // properly set the span id and the parent span id.
    SpanContext context;

    context.populateFromString(request_headers.OtSpanContext()->value().c_str());

    // Create either a child or a shared-context Zipkin span.
    //
    // An all-new child span will be started if the current context carries the SR annotation. In
    // this case, we are dealing with an egress operation that causally succeeds a previous
    // ingress operation. This envoy instance will be the the client-side of the new span, to which
    // it will add the CS annotation.
    //
    // Differently, a shared-context span will be created if the current context carries the CS
    // annotation. In this case, we are dealing with an ingress operation. This envoy instance,
    // being at the receiving end, will add the SR annotation to the shared span context.

    new_zipkin_span =
        tracer.startSpan(config, request_headers.Host()->value().c_str(), start_time, context);
  } else {
    // Create a root Zipkin span. No context was found in the headers.
    new_zipkin_span = tracer.startSpan(config, request_headers.Host()->value().c_str(), start_time);
  }

  ZipkinSpanPtr active_span(new ZipkinSpan(*new_zipkin_span, tracer));
  return std::move(active_span);
}

ReporterImpl::ReporterImpl(Driver& driver, Event::Dispatcher& dispatcher,
                           const std::string& collector_endpoint)
    : driver_(driver), collector_endpoint_(collector_endpoint) {
  flush_timer_ = dispatcher.createTimer([this]() -> void {
    driver_.tracerStats().timer_flushed_.inc();
    flushSpans();
    enableTimer();
  });

  const uint64_t min_flush_spans =
      driver_.runtime().snapshot().getInteger("tracing.zipkin.min_flush_spans", 5U);
  span_buffer_.allocateBuffer(min_flush_spans);

  enableTimer();
}

ReporterPtr ReporterImpl::NewInstance(Driver& driver, Event::Dispatcher& dispatcher,
                                      const std::string& collector_endpoint) {
  return ReporterPtr(new ReporterImpl(driver, dispatcher, collector_endpoint));
}

// TODO(fabolive): Need to avoid the copy to improve performance.
void ReporterImpl::reportSpan(const Span& span) {
  span_buffer_.addSpan(span);

  const uint64_t min_flush_spans =
      driver_.runtime().snapshot().getInteger("tracing.zipkin.min_flush_spans", 5U);

  if (span_buffer_.pendingSpans() == min_flush_spans) {
    flushSpans();
  }
}

void ReporterImpl::enableTimer() {
  const uint64_t flush_interval =
      driver_.runtime().snapshot().getInteger("tracing.zipkin.flush_interval_ms", 5000U);
  flush_timer_->enableTimer(std::chrono::milliseconds(flush_interval));
}

void ReporterImpl::flushSpans() {
  if (span_buffer_.pendingSpans()) {
    driver_.tracerStats().spans_sent_.add(span_buffer_.pendingSpans());

    const std::string request_body = span_buffer_.toStringifiedJsonArray();
    Http::MessagePtr message(new Http::RequestMessageImpl());
    message->headers().insertMethod().value().setReference(Http::Headers::get().MethodValues.Post);
    message->headers().insertPath().value(collector_endpoint_);
    message->headers().insertHost().value(driver_.cluster()->name());
    message->headers().insertContentType().value().setReference(
        Http::Headers::get().ContentTypeValues.Json);

    Buffer::InstancePtr body(new Buffer::OwnedImpl());
    body->add(request_body);
    message->body() = std::move(body);

    const uint64_t timeout =
        driver_.runtime().snapshot().getInteger("tracing.zipkin.request_timeout", 5000U);
    driver_.clusterManager()
        .httpAsyncClientForCluster(driver_.cluster()->name())
        .send(std::move(message), *this, std::chrono::milliseconds(timeout));

    span_buffer_.clear();
  }
}

void ReporterImpl::onFailure(Http::AsyncClient::FailureReason) {
  driver_.tracerStats().reports_failed_.inc();
}

void ReporterImpl::onSuccess(Http::MessagePtr&& http_response) {
  if (Http::Utility::getResponseStatus(http_response->headers()) !=
      enumToInt(Http::Code::Accepted)) {
    driver_.tracerStats().reports_dropped_.inc();
  } else {
    driver_.tracerStats().reports_sent_.inc();
  }
}
} // namespace Zipkin
} // namespace Envoy
