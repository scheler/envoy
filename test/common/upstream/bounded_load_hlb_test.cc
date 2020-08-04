#include <memory>

#include "envoy/config/cluster/v3/cluster.pb.h"

#include "common/upstream/thread_aware_lb_impl.h"

#include "test/common/upstream/utility.h"
#include "test/mocks/upstream/mocks.h"

namespace Envoy {
namespace Upstream {
namespace {

class TestHashingLoadBalancer : public ThreadAwareLoadBalancerBase::HashingLoadBalancer {
public:
  TestHashingLoadBalancer(NormalizedHostWeightVectorPtr normalized_host_weights = nullptr)
      : normalized_host_weights_(normalized_host_weights) {}

  HostConstSharedPtr chooseHost(uint64_t hash, uint32_t /* attempt */) const {
    if (normalized_host_weights_ == nullptr) {
      return nullptr;
    }
    return normalized_host_weights_->at(hash).first;
  }
  NormalizedHostWeightVectorPtr normalized_host_weights_;

private:
};

class BoundedLoadHashingLoadBalancerTest : public testing::Test {
public:
  ThreadAwareLoadBalancerBase::HostOverloadedPredicate getHostOverloadedPredicate(bool always) {
    bool always_return = always;
    return [&always_return](HostConstSharedPtr, double) -> bool { return always_return; };
  }
  ThreadAwareLoadBalancerBase::HostOverloadedPredicate
  getHostOverloadedPredicate(HostConstSharedPtr overloaded_host) {
    HostConstSharedPtr host = overloaded_host;
    return [&host](HostConstSharedPtr h, double) -> bool { return h == host; };
  }

  ThreadAwareLoadBalancerBase::HostOverloadedPredicate
  getHostOverloadedPredicate(const std::vector<std::string> addresses) {
    std::vector<std::string> hosts_overloaded = addresses;
    return [hosts_overloaded](HostConstSharedPtr h, double) -> bool {
      for (std::string host : hosts_overloaded) {
        if (host.compare(h->address()->asString()) == 0) {
          return true;
        }
      }
      return false;
    };
  }

  NormalizedHostWeightVectorPtr createHosts(uint32_t num_hosts) {
    const double equal_weight = static_cast<double>(1.0 / num_hosts);
    std::cout << equal_weight << std::endl;
    std::shared_ptr<NormalizedHostWeightVector> vector =
        std::make_shared<NormalizedHostWeightVector>();
    for (uint32_t i = 0; i < num_hosts; i++) {
      vector->push_back(
          {makeTestHost(info_, fmt::format("tcp://127.0.0.1{}:90", i)), equal_weight});
    }
    return vector;
  }

  std::pair<NormalizedHostWeightVectorPtr, NormalizedHostWeightVectorPtr>
  createHostsMappedByMultipleHosts(uint32_t num_hosts) {
    const double equal_weight = static_cast<double>(1.0 / num_hosts);
    std::cout << equal_weight << std::endl;
    std::shared_ptr<NormalizedHostWeightVector> hosts =
        std::make_shared<NormalizedHostWeightVector>();
    std::shared_ptr<NormalizedHostWeightVector> ring =
        std::make_shared<NormalizedHostWeightVector>();
    for (uint32_t i = 0; i < num_hosts; i++) {
      HostConstSharedPtr h = makeTestHost(info_, fmt::format("tcp://127.0.0.1{}:90", i));
      ring->push_back({h, equal_weight});
      ring->push_back({h, equal_weight});
      hosts->push_back({h, equal_weight});
    }
    return {hosts, ring};
  }

  ThreadAwareLoadBalancerBase::HashingLoadBalancerSharedPtr hlb_;
  std::unique_ptr<ThreadAwareLoadBalancerBase::BoundedLoadHashingLoadBalancer> lb_;
  std::shared_ptr<MockClusterInfo> info_{new NiceMock<MockClusterInfo>()};

  ThreadAwareLoadBalancerBase::HostOverloadedPredicate hostOverLoadedPredicate;
};

// Works correctly when hash balance factor is 0, when balancing is not required.
TEST_F(BoundedLoadHashingLoadBalancerTest, HashBalanceDisabled) {
  // EXPECT_DEATH(init(0), "");
  ThreadAwareLoadBalancerBase::HashingLoadBalancerSharedPtr hlb =
      std::make_shared<TestHashingLoadBalancer>();
  EXPECT_DEATH(std::make_unique<ThreadAwareLoadBalancerBase::BoundedLoadHashingLoadBalancer>(
                   hlb, nullptr, 0),
               "");
};

// Works correctly without any hosts (nullptr or empty vector).
TEST_F(BoundedLoadHashingLoadBalancerTest, NoHosts) {
  hlb_ = std::make_shared<TestHashingLoadBalancer>();
  EXPECT_DEATH(std::make_unique<ThreadAwareLoadBalancerBase::BoundedLoadHashingLoadBalancer>(
                   hlb_, nullptr, 1),
               "");

  NormalizedHostWeightVectorPtr normalized_host_weights_empty =
      std::make_shared<NormalizedHostWeightVector>();
  lb_ = std::make_unique<ThreadAwareLoadBalancerBase::BoundedLoadHashingLoadBalancer>(
      hlb_, normalized_host_weights_empty, 1);
  EXPECT_EQ(lb_->chooseHost(1, 1), nullptr);
};

// Works correctly without any hashing load balancer.
TEST_F(BoundedLoadHashingLoadBalancerTest, NoHashingLoadBalancer) {
  NormalizedHostWeightVectorPtr normalized_host_weights_empty =
      std::make_shared<NormalizedHostWeightVector>();
  lb_ = std::make_unique<ThreadAwareLoadBalancerBase::BoundedLoadHashingLoadBalancer>(
      nullptr, normalized_host_weights_empty, 1);

  EXPECT_EQ(lb_->chooseHost(1, 1), nullptr);
};

// Works correctly for the case when no host is ever overloaded.
TEST_F(BoundedLoadHashingLoadBalancerTest, NoHostEverOverloaded) {

  // setup: 5 hosts, none ever overloaded.
  NormalizedHostWeightVectorPtr normalized_host_weights = createHosts(5);
  hostOverLoadedPredicate = getHostOverloadedPredicate(false);
  hlb_ = std::make_shared<TestHashingLoadBalancer>(normalized_host_weights);
  lb_ = std::make_unique<ThreadAwareLoadBalancerBase::BoundedLoadHashingLoadBalancer>(
      hlb_, normalized_host_weights, 1, hostOverLoadedPredicate);

  // test
  for (uint32_t i = 0; i < 5; i++) {
    HostConstSharedPtr host = lb_->chooseHost(i, 1);
    EXPECT_NE(host, nullptr);
    EXPECT_EQ(host->address()->asString(), fmt::format("127.0.0.1{}:90", i));
  }
};

// Works correctly for the case one host is overloaded.
TEST_F(BoundedLoadHashingLoadBalancerTest, OneHostOverloaded) {
  /*
    In this host 2 is overloaded. The random shuffle sequence of 5
    elements with seed 2 is 3 1 4 0 2. When the host picked up for
    hash 2 (which is 127.0.0.12) is overloaded, host 3 (127.0.0.13)
    is picked up,
  */

  // setup: 5 hosts, one of them is overloaded.
  NormalizedHostWeightVectorPtr normalized_host_weights = createHosts(5);
  std::vector<std::string> addresses;
  addresses.push_back("127.0.0.12:90");

  hostOverLoadedPredicate = getHostOverloadedPredicate(addresses);
  hlb_ = std::make_shared<TestHashingLoadBalancer>(normalized_host_weights);
  lb_ = std::make_unique<ThreadAwareLoadBalancerBase::BoundedLoadHashingLoadBalancer>(
      hlb_, normalized_host_weights, 1, hostOverLoadedPredicate);

  // test
  HostConstSharedPtr host = lb_->chooseHost(2, 1);
  EXPECT_NE(host, nullptr);
  EXPECT_EQ(host->address()->asString(), "127.0.0.13:90");
};

// Works correctly for the case a few hosts are overloaded.
TEST_F(BoundedLoadHashingLoadBalancerTest, MultipleHostOverloaded) {
  /*
    In this case hosts 1, 2 & 3 are overloaded. The random shuffle
    sequence of 5 elements with seed 2 is 3 1 4 0 2. When the host
    picked up for hash 2 (which is 127.0.0.12) is overloaded, the
    method passes over hosts 3 & 1 and picks host 4 (127.0.0.10)
    is picked up,

  */

  // setup: 5 hosts, few of them are overloaded.
  NormalizedHostWeightVectorPtr normalized_host_weights = createHosts(5);
  std::vector<std::string> addresses;
  addresses.push_back("127.0.0.11:90");
  addresses.push_back("127.0.0.12:90");
  addresses.push_back("127.0.0.13:90");

  hostOverLoadedPredicate = getHostOverloadedPredicate(addresses);
  hlb_ = std::make_shared<TestHashingLoadBalancer>(normalized_host_weights);
  lb_ = std::make_unique<ThreadAwareLoadBalancerBase::BoundedLoadHashingLoadBalancer>(
      hlb_, normalized_host_weights, 1, hostOverLoadedPredicate);

  // test
  HostConstSharedPtr host = lb_->chooseHost(2, 1);
  EXPECT_NE(host, nullptr);
  EXPECT_EQ(host->address()->asString(), "127.0.0.14:90");
};

// Works correctly for the case a few hosts are overloaded.
TEST_F(BoundedLoadHashingLoadBalancerTest, MultipleHashSameHostOverloaded) {
  /*
    In this case hosts 2 is overloaded. The random shuffle
    sequence of 5 elements with seed 2 is 3 1 4 0 2. When the host
    picked up for hash 2 (which is 127.0.0.12) is overloaded, the
    method passes over hosts 3 & 1 and picks host 4 (127.0.0.10)
    is picked up,

  */
  // setup: 5 hosts, one of them is overloaded.
  std::pair<NormalizedHostWeightVectorPtr, NormalizedHostWeightVectorPtr> pair =
      createHostsMappedByMultipleHosts(5);
  NormalizedHostWeightVectorPtr normalized_host_weights = pair.first;
  NormalizedHostWeightVectorPtr hosts_on_ring = pair.second;
  std::vector<std::string> addresses;
  addresses.push_back("127.0.0.12:90");

  ThreadAwareLoadBalancerBase::HostOverloadedPredicate hostOverLoaded =
      getHostOverloadedPredicate(addresses);
  ThreadAwareLoadBalancerBase::HashingLoadBalancerSharedPtr hlb =
      std::make_shared<TestHashingLoadBalancer>(hosts_on_ring);
  lb_ = std::make_unique<ThreadAwareLoadBalancerBase::BoundedLoadHashingLoadBalancer>(
      hlb, normalized_host_weights, 1, hostOverLoaded);

  // test
  HostConstSharedPtr host1 = lb_->chooseHost(4, 1);
  EXPECT_NE(host1, nullptr);
  HostConstSharedPtr host2 = lb_->chooseHost(5, 1);
  EXPECT_NE(host2, nullptr);

  // they are different
  EXPECT_NE(host1->address()->asString(), host2->address()->asString());

  // sequence for 4 is 34021;
  EXPECT_EQ(host1->address()->asString(), "127.0.0.13:90");
  // sequence for 5 is 20134
  EXPECT_EQ(host2->address()->asString(), "127.0.0.10:90");
};

} // namespace
} // namespace Upstream
} // namespace Envoy
