#include "ssl_integration_test.h"

#include <memory>
#include <string>

#include "common/event/dispatcher_impl.h"
#include "common/network/utility.h"
#include "common/ssl/context_config_impl.h"
#include "common/ssl/context_manager_impl.h"

#include "test/integration/ssl_utility.h"
#include "test/test_common/network_utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "integration.h"
#include "utility.h"

using testing::Return;

namespace Envoy {
namespace Ssl {

void SslIntegrationTest::initialize() {
  BaseIntegrationTest::initialize();

  runtime_.reset(new NiceMock<Runtime::MockLoader>());
  context_manager_.reset(new ContextManagerImpl(*runtime_));
  upstream_ssl_ctx_ = createUpstreamSslContext();
  fake_upstreams_.emplace_back(
      new FakeUpstream(upstream_ssl_ctx_.get(), 0, FakeHttpConnection::Type::HTTP1, version_));
  registerPort("upstream_0", fake_upstreams_.back()->localAddress()->ip()->port());
  fake_upstreams_.emplace_back(
      new FakeUpstream(upstream_ssl_ctx_.get(), 0, FakeHttpConnection::Type::HTTP1, version_));
  registerPort("upstream_1", fake_upstreams_.back()->localAddress()->ip()->port());
  test_server_ = MockRuntimeIntegrationTestServer::create(
      TestEnvironment::temporaryFileSubstitute("test/config/integration/server_ssl.json", port_map_,
                                               version_),
      version_);
  registerTestServerPorts({"http"});
  client_ssl_ctx_plain_ = createClientSslContext(false, false, *context_manager_);
  client_ssl_ctx_alpn_ = createClientSslContext(true, false, *context_manager_);
  client_ssl_ctx_san_ = createClientSslContext(false, true, *context_manager_);
  client_ssl_ctx_alpn_san_ = createClientSslContext(true, true, *context_manager_);
}

void SslIntegrationTest::TearDown() {
  test_server_.reset();
  fake_upstreams_.clear();
  upstream_ssl_ctx_.reset();
  client_ssl_ctx_plain_.reset();
  client_ssl_ctx_alpn_.reset();
  client_ssl_ctx_san_.reset();
  client_ssl_ctx_alpn_san_.reset();
  context_manager_.reset();
  runtime_.reset();
}

ServerContextPtr SslIntegrationTest::createUpstreamSslContext() {
  static auto* upstream_stats_store = new Stats::TestIsolatedStoreImpl();
  std::string json = R"EOF(
{
  "cert_chain_file": "{{ test_rundir }}/test/config/integration/certs/upstreamcert.pem",
  "private_key_file": "{{ test_rundir }}/test/config/integration/certs/upstreamkey.pem"
}
)EOF";

  Json::ObjectSharedPtr loader = TestEnvironment::jsonLoadFromString(json);
  ServerContextConfigImpl cfg(*loader);
  return context_manager_->createSslServerContext(*upstream_stats_store, cfg);
}

Network::ClientConnectionPtr SslIntegrationTest::makeSslClientConnection(bool alpn, bool san) {
  Network::Address::InstanceConstSharedPtr address = getSslAddress(version_, lookupPort("http"));
  if (alpn) {
    return dispatcher_->createSslClientConnection(
        san ? *client_ssl_ctx_alpn_san_ : *client_ssl_ctx_alpn_, address,
        Network::Address::InstanceConstSharedPtr());
  } else {
    return dispatcher_->createSslClientConnection(
        san ? *client_ssl_ctx_san_ : *client_ssl_ctx_plain_, address,
        Network::Address::InstanceConstSharedPtr());
  }
}

void SslIntegrationTest::checkStats() {
  if (version_ == Network::Address::IpVersion::v4) {
    Stats::CounterSharedPtr counter = test_server_->counter("listener.127.0.0.1_0.ssl.handshake");
    EXPECT_EQ(1U, counter->value());
    counter->reset();
  } else {
    // ':' is a reserved char in statsd.
    Stats::CounterSharedPtr counter = test_server_->counter("listener.[__1]_0.ssl.handshake");
    EXPECT_EQ(1U, counter->value());
    counter->reset();
  }
}

INSTANTIATE_TEST_CASE_P(IpVersions, SslIntegrationTest,
                        testing::ValuesIn(TestEnvironment::getIpVersionsForTest()));

TEST_P(SslIntegrationTest, RouterRequestAndResponseWithGiantBodyBuffer) {
  ConnectionCreationFunction creator = [&]() -> Network::ClientConnectionPtr {
    return makeSslClientConnection(false, false);
  };
  testRouterRequestAndResponseWithBody(16 * 1024 * 1024, 16 * 1024 * 1024, false, &creator);
  checkStats();
}

TEST_P(SslIntegrationTest, RouterRequestAndResponseWithBodyNoBuffer) {
  ConnectionCreationFunction creator = [&]() -> Network::ClientConnectionPtr {
    return makeSslClientConnection(false, false);
  };
  testRouterRequestAndResponseWithBody(1024, 512, false, &creator);
  checkStats();
}

TEST_P(SslIntegrationTest, RouterRequestAndResponseWithBodyNoBufferHttp2) {
  setDownstreamProtocol(Http::CodecClient::Type::HTTP2);
  ConnectionCreationFunction creator = [&]() -> Network::ClientConnectionPtr {
    return makeSslClientConnection(true, false);
  };
  testRouterRequestAndResponseWithBody(1024, 512, false, &creator);
  checkStats();
}

TEST_P(SslIntegrationTest, RouterRequestAndResponseWithBodyNoBufferVierfySAN) {
  ConnectionCreationFunction creator = [&]() -> Network::ClientConnectionPtr {
    return makeSslClientConnection(false, true);
  };
  testRouterRequestAndResponseWithBody(1024, 512, false, &creator);
  checkStats();
}

TEST_P(SslIntegrationTest, RouterRequestAndResponseWithBodyNoBufferHttp2VerifySAN) {
  setDownstreamProtocol(Http::CodecClient::Type::HTTP2);
  ConnectionCreationFunction creator = [&]() -> Network::ClientConnectionPtr {
    return makeSslClientConnection(true, true);
  };
  testRouterRequestAndResponseWithBody(1024, 512, false, &creator);
  checkStats();
}

TEST_P(SslIntegrationTest, RouterHeaderOnlyRequestAndResponse) {
  ConnectionCreationFunction creator = [&]() -> Network::ClientConnectionPtr {
    return makeSslClientConnection(false, false);
  };
  testRouterHeaderOnlyRequestAndResponse(true, &creator);
  checkStats();
}

TEST_P(SslIntegrationTest, RouterUpstreamDisconnectBeforeResponseComplete) {
  ConnectionCreationFunction creator = [&]() -> Network::ClientConnectionPtr {
    return makeSslClientConnection(false, false);
  };
  testRouterUpstreamDisconnectBeforeResponseComplete(&creator);
  checkStats();
}

TEST_P(SslIntegrationTest, RouterDownstreamDisconnectBeforeRequestComplete) {
  ConnectionCreationFunction creator = [&]() -> Network::ClientConnectionPtr {
    return makeSslClientConnection(false, false);
  };
  testRouterDownstreamDisconnectBeforeRequestComplete(&creator);
  checkStats();
}

TEST_P(SslIntegrationTest, RouterDownstreamDisconnectBeforeResponseComplete) {
  ConnectionCreationFunction creator = [&]() -> Network::ClientConnectionPtr {
    return makeSslClientConnection(false, false);
  };
  testRouterDownstreamDisconnectBeforeResponseComplete(&creator);
  checkStats();
}

// This test must be here vs integration_admin_test so that it tests a server with loaded certs.
TEST_P(SslIntegrationTest, AdminCertEndpoint) {
  initialize();
  BufferingStreamDecoderPtr response = IntegrationUtil::makeSingleRequest(
      lookupPort("admin"), "GET", "/certs", "", downstreamProtocol(), version_);
  EXPECT_TRUE(response->complete());
  EXPECT_STREQ("200", response->headers().Status()->value().c_str());
}

TEST_P(SslIntegrationTest, AltAlpn) {
  ConnectionCreationFunction creator = [&]() -> Network::ClientConnectionPtr {
    return makeSslClientConnection(true, false);
  };
  initialize();
  // Connect with ALPN, but we should end up using HTTP/1.
  MockRuntimeIntegrationTestServer* server =
      dynamic_cast<MockRuntimeIntegrationTestServer*>(test_server_.get());
  ON_CALL(server->runtime_->snapshot_, featureEnabled("ssl.alt_alpn", 0))
      .WillByDefault(Return(true));
  testRouterRequestAndResponseWithBody(1024, 512, false, &creator);
  checkStats();
}

} // namespace Ssl
} // namespace Envoy
