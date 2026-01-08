#pragma once

#include <boost/asio/buffer.hpp>
#include <boost/system/system_error.hpp>

class StreamLayer {
public:
    virtual ~StreamLayer() = default;

    virtual void async_handshake(std::function<void(boost::system::error_code)> handler) = 0;

    virtual void async_read_fully(boost::asio::mutable_buffer buffer,
                                  std::function<void(boost::system::error_code, std::size_t)> handler) = 0;

    virtual void async_write_fully(boost::asio::const_buffer buffer,
                                   std::function<void(boost::system::error_code, std::size_t)> handler) = 0;

    virtual void cancel() = 0;

    virtual void close() = 0;

    virtual std::string remote_address() const = 0;

    virtual bool is_open() const = 0;
};
