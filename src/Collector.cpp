#include "Collector.h"

#include <ios>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <functional>

#include <glog/logging.h>

#include <boost/smart_ptr.hpp>

#include <thrift/transport/TBufferTransports.h>
#include <thrift/protocol/TBinaryProtocol.h>

#define RAPIDJSON_HAS_STDSTRING 1
#include <rapidjson/stringbuffer.h>
#include <rapidjson/prettywriter.h>

#include "../gen-cpp/zipkinCore_types.h"

#include "Tracer.h"

namespace zipkin
{

struct SpanDeliveryReporter : public RdKafka::DeliveryReportCb
{
    void dr_cb(RdKafka::Message &message)
    {
        CachedSpan *span = static_cast<CachedSpan *>(message.msg_opaque());

        if (RdKafka::ErrorCode::ERR_NO_ERROR == message.err())
        {
            VLOG(2) << "Deliveried Span `" << std::hex << span->id()
                    << "` to topic " << message.topic_name()
                    << " #" << message.partition() << " @" << message.offset()
                    << " with " << message.len() << " bytes";
        }
        else
        {
            LOG(WARNING) << "Fail to delivery Span `" << std::hex << span->id()
                         << "` to topic " << message.topic_name()
                         << " #" << message.partition() << " @" << message.offset()
                         << ", " << message.errstr();
        }

        span->release();
    }
};

struct HashPartitioner : public RdKafka::PartitionerCb
{
    std::hash<std::string> hasher;

    virtual int32_t partitioner_cb(const RdKafka::Topic *topic,
                                   const std::string *key,
                                   int32_t partition_cnt,
                                   void *msg_opaque) override
    {
        return hasher(*key) % partition_cnt;
    }
};

class ReusableMemoryBuffer : public apache::thrift::transport::TMemoryBuffer
{
  public:
    ReusableMemoryBuffer(CachedSpan *cached_span)
        : TMemoryBuffer(cached_span->cache_ptr(), cached_span->cache_size())
    {
        resetBuffer();

        size_t size = cached_span->cache_size();

        wBound_ += size;
        bufferSize_ = size;
    }
};

void KafkaCollector::submit(Span *span)
{
    boost::shared_ptr<apache::thrift::transport::TMemoryBuffer> buf(new ReusableMemoryBuffer(static_cast<CachedSpan *>(span)));

    uint32_t wrote = 0;

    switch (m_message_codec)
    {
    case MessageCodec::binary:
    {
        apache::thrift::protocol::TBinaryProtocol protocol(buf);

        wrote = protocol.writeByte(12) + // type of the list elements: 12 == struct
                protocol.writeI32(1);    // count of spans that will follow

        wrote += span->serialize_binary(protocol);

        break;
    }

    case MessageCodec::json:
    case MessageCodec::pretty_json:
    {
        rapidjson::StringBuffer buffer;

        if (m_message_codec == MessageCodec::pretty_json) {
            rapidjson::PrettyWriter <rapidjson::StringBuffer> writer(buffer);

            writer.StartArray();
            span->serialize_json(writer);
            writer.EndArray(1);
        } else {
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);

            writer.StartArray();
            span->serialize_json(writer);
            writer.EndArray(1);
        }

        buf->write((const uint8_t *)buffer.GetString(), buffer.GetSize());

        wrote =  buffer.GetSize();

        break;
    }
    }

    VLOG(2) << "Span @ " << span << " wrote " << wrote << " bytes to message, id=" << std::hex << span->id();
    VLOG(3) << span->message();

    uint8_t *ptr = nullptr;
    uint32_t len = 0;

    buf->getBuffer(&ptr, &len);

    assert(ptr);
    assert(wrote == len);

    RdKafka::ErrorCode err = m_producer->produce(m_topic.get(),
                                                 m_partition,
                                                 0,             // msgflags
                                                 (void *)ptr,   // payload
                                                 len,           // payload length
                                                 &span->name(), // key
                                                 span);         // msg_opaque

    if (RdKafka::ErrorCode::ERR_NO_ERROR != err)
    {
        LOG(WARNING) << "fail to submit message to Kafka, " << err2str(err);
    }
    else
    {
        m_producer->poll(0);
    }
}

const std::string to_string(CompressionCodec codec)
{
    switch (codec)
    {
    case CompressionCodec::none:
        return "none";
    case CompressionCodec::gzip:
        return "gzip";
    case CompressionCodec::snappy:
        return "snappy";
    case CompressionCodec::lz4:
        return "lz4";
    }
}

const std::string to_string(MessageCodec codec)
{
    switch (codec)
    {
    case MessageCodec::binary:
        return "binary";
    case MessageCodec::json:
        return "json";
    case MessageCodec::pretty_json:
        return "pretty_json";
    }
}

bool kafka_conf_set(std::unique_ptr<RdKafka::Conf> &conf, const std::string &name, const std::string &value)
{
    std::string errstr;

    bool ok = RdKafka::Conf::CONF_OK == conf->set(name, value, errstr);

    if (!ok)
    {
        LOG(ERROR) << "fail to set " << name << " to " << value << ", " << errstr;
    }

    return ok;
}

KafkaCollector *KafkaConf::create(void) const
{
    std::string errstr;

    std::unique_ptr<RdKafka::Conf> producer_conf(RdKafka::Conf::create(RdKafka::Conf::ConfType::CONF_GLOBAL));
    std::unique_ptr<RdKafka::Conf> topic_conf(RdKafka::Conf::create(RdKafka::Conf::ConfType::CONF_TOPIC));
    std::unique_ptr<RdKafka::DeliveryReportCb> reporter(new SpanDeliveryReporter());
    std::unique_ptr<RdKafka::PartitionerCb> partitioner;

    if (!kafka_conf_set(producer_conf, "metadata.broker.list", brokers))
        return nullptr;

    if (RdKafka::Conf::CONF_OK != producer_conf->set("dr_cb", reporter.get(), errstr))
    {
        LOG(ERROR) << "fail to set delivery reporter, " << errstr;
        return nullptr;
    }

    if (compression_codec != CompressionCodec::none && !kafka_conf_set(producer_conf, "compression.codec", to_string(compression_codec)))
        return nullptr;

    if (batch_num_messages && !kafka_conf_set(producer_conf, "batch.num.messages", std::to_string(batch_num_messages)))
        return nullptr;

    if (queue_buffering_max_messages && !kafka_conf_set(producer_conf, "queue.buffering.max.messages", std::to_string(queue_buffering_max_messages)))
        return nullptr;

    if (queue_buffering_max_kbytes && !kafka_conf_set(producer_conf, "queue.buffering.max.kbytes", std::to_string(queue_buffering_max_kbytes)))
        return nullptr;

    if (queue_buffering_max_ms.count() && !kafka_conf_set(producer_conf, "queue.buffering.max.ms", std::to_string(queue_buffering_max_ms.count())))
        return nullptr;

    if (message_send_max_retries && !kafka_conf_set(producer_conf, "message.send.max.retries", std::to_string(message_send_max_retries)))
        return nullptr;

    if (partition == RdKafka::Topic::PARTITION_UA)
    {
        partitioner.reset(new HashPartitioner());

        if (RdKafka::Conf::CONF_OK != topic_conf->set("partitioner_cb", partitioner.get(), errstr))
        {
            LOG(ERROR) << "fail to set partitioner, " << errstr;
            return nullptr;
        }
    }

    if (VLOG_IS_ON(2))
    {
        VLOG(2) << "# Global config";

        std::unique_ptr<std::list<std::string>> producer_conf_items(producer_conf->dump());

        for (auto it = producer_conf_items->begin(); it != producer_conf_items->end();)
        {
            VLOG(2) << *it++ << " = " << *it++;
        }

        VLOG(2) << "# Topic config";

        std::unique_ptr<std::list<std::string>> topic_conf_items(topic_conf->dump());

        for (auto it = topic_conf_items->begin(); it != topic_conf_items->end();)
        {
            VLOG(2) << *it++ << " = " << *it++;
        }
    }

    std::unique_ptr<RdKafka::Producer> producer(RdKafka::Producer::create(producer_conf.get(), errstr));

    if (!producer)
    {
        LOG(ERROR) << "fail to connect Kafka broker @ " << brokers << ", " << errstr;

        return nullptr;
    }

    std::unique_ptr<RdKafka::Topic> topic(RdKafka::Topic::create(producer.get(), topic_name, topic_conf.get(), errstr));

    if (!topic)
    {
        LOG(ERROR) << "fail to create topic `" << topic_name << "`, " << errstr;

        return nullptr;
    }

    return new KafkaCollector(producer, topic, std::move(reporter), std::move(partitioner), partition, message_codec);
}

} // namespace zipkin