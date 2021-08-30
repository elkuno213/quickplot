// include implot.h for ImPlotPoint struct, to avoid copies when plotting
#include "implot.h" // NOLINT

#include "quickplot/message_parser.hpp"
#include <libstatistics_collector/moving_average_statistics/moving_average.hpp>
#include <mutex>
#include <string>
#include <utility>
#include <unordered_map>
#include <memory>
#include <deque>
#include <list>
#include <vector>
#include <boost/circular_buffer.hpp>

namespace quickplot
{
using std::placeholders::_1;
using CircularBuffer = boost::circular_buffer<ImPlotPoint>;
using libstatistics_collector::moving_average_statistics::MovingAverageStatistics;
using libstatistics_collector::moving_average_statistics::StatisticData;

class PlotDataBuffer;

// Mutex-protected access to an immutable random-access-iterator of plot data
class PlotDataContainer
{
private:
  const PlotDataBuffer* parent_;
  std::unique_lock<std::mutex> lock_;

public:
  explicit PlotDataContainer(const PlotDataBuffer* parent);

  size_t size() const;

  CircularBuffer::const_iterator begin() const;

  CircularBuffer::const_iterator end() const;
};

// Provide mutex-protected access to a circular buffer of ImPlotPoint
class PlotDataBuffer
{
  friend class PlotDataContainer;

private:
  // lock this mutex when accessing data from the plot buffer
  mutable std::mutex mutex_;
  CircularBuffer data_;

public:
  explicit PlotDataBuffer(size_t capacity)
  : mutex_(), data_(capacity)
  {

  }

  PlotDataContainer data() const
  {
    return PlotDataContainer(this);
  }

  bool empty() const
  {
    std::unique_lock<std::mutex> lock(mutex_);
    return data_.empty();
  }

  void push(double x, double y)
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (data_.full()) {
      data_.set_capacity(data_.capacity() * 2);
    }
    data_.push_back(
      ImPlotPoint(
        x,
        y));
  }

  void clear_data_up_to(rclcpp::Time t)
  {
    std::unique_lock<std::mutex> lock(mutex_);
    auto s = t.seconds();
    while (!data_.empty()) {
      if (data_.front().x < s) {
        data_.pop_front();
      } else {
        break;
      }
    }
  }

  void clear()
  {
    std::unique_lock<std::mutex> lock(mutex_);
    data_.clear();
  }
};

PlotDataContainer::PlotDataContainer(const PlotDataBuffer* parent)
: parent_(parent), lock_(parent_->mutex_)
{

}

size_t PlotDataContainer::size() const
{
  return parent_->data_.size();
}

CircularBuffer::const_iterator PlotDataContainer::begin() const
{
  return parent_->data_.begin();
}

CircularBuffer::const_iterator PlotDataContainer::end() const
{
  return parent_->data_.end();
}

class PlotSubscription
{
private:
  std::vector<uint8_t> message_buffer_;
  std::shared_ptr<IntrospectionMessageDeserializer> deserializer_;
  rclcpp::node_interfaces::NodeClockInterface::SharedPtr node_clock_interface_;
  rclcpp::GenericSubscription::SharedPtr subscription_;

  rclcpp::Time last_received_;
  rclcpp::Clock steady_clock_{RCL_STEADY_TIME};
  MovingAverageStatistics receive_period_stats_;

  // protect access to list of buffers
  mutable std::mutex buffers_mutex_;

public:
  // One data buffer per plotted member of a message.
  //
  // Using list instead of vector, since the emplace_back operation does not require
  // the element to be MoveConstructible for resizing the array. The data buffer should
  // not be move constructed.
  std::list<std::pair<MessageMember, PlotDataBuffer>> buffers_;

  explicit PlotSubscription(
    std::string topic_name,
    rclcpp::node_interfaces::NodeTopicsInterface::SharedPtr topics_interface,
    rclcpp::node_interfaces::NodeClockInterface::SharedPtr clock_interface,
    std::shared_ptr<IntrospectionMessageDeserializer> deserializer)
  : deserializer_(deserializer), node_clock_interface_(clock_interface)
  {
    message_buffer_ = deserializer_->init_buffer();
    subscription_ = rclcpp::create_generic_subscription(
      topics_interface,
      topic_name,
      deserializer_->message_type(),
      rclcpp::SensorDataQoS(),
      std::bind(&PlotSubscription::receive_callback, this, _1),
      rclcpp::SubscriptionOptionsWithAllocator<std::allocator<void>>()
    );
  }

  ~PlotSubscription()
  {
    deserializer_->fini_buffer(message_buffer_);
  }

  // disable copy and move
  PlotSubscription & operator=(PlotSubscription && other) = delete;

  void add_field(MessageMember in_member, size_t capacity)
  {
    std::unique_lock<std::mutex> lock(buffers_mutex_);
    for (auto & [member, buffer] : buffers_) {
      if (member.path == in_member.path) {
        std::invalid_argument("member already in subscription");
      }
    }
    buffers_.emplace_back(
      in_member,
      capacity
    );
  }

  PlotDataBuffer & get_buffer(std::vector<std::string> member_path)
  {
    std::unique_lock<std::mutex> lock(buffers_mutex_);
    for (auto & [member, buffer] : buffers_) {
      if (member.path == member_path) {
        return buffer;
      }
    }
    throw std::invalid_argument("member not found by path");
  }

  StatisticData receive_period_stats() const
  {
    return receive_period_stats_.GetStatistics();
  }

  void receive_callback(std::shared_ptr<rclcpp::SerializedMessage> message)
  {
    auto t_steady = steady_clock_.now();
    if (last_received_.nanoseconds() != 0) {
      receive_period_stats_.AddMeasurement((t_steady - last_received_).seconds());
    }
    last_received_ = t_steady;

    deserializer_->deserialize(*message, message_buffer_.data());
    auto stamp = deserializer_->get_header_stamp(message_buffer_.data());
    rclcpp::Time t;
    if (stamp.has_value()) {
      t = stamp.value();
    } else {
      t = node_clock_interface_->get_clock()->now();
    }
    {
      std::unique_lock<std::mutex> lock(buffers_mutex_);
      for (auto & [member, buffer] : buffers_) {
        double value = deserializer_->get_numeric(message_buffer_.data(), member.info);
        buffer.push(t.seconds(), value);
      }
    }
  }

  void clear_all_data()
  {
    std::unique_lock<std::mutex> lock(buffers_mutex_);
    for (auto & [_, buffer] : buffers_) {
      buffer.clear();
    }
  }
};

} // namespace quickplot
