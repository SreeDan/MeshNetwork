#pragma once

#include <algorithm>
#include <deque>
#include <memory>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/write.hpp>

template<typename AsyncWriteStream>
class AsyncWriteQueue :
        public std::enable_shared_from_this<AsyncWriteQueue<AsyncWriteStream> > {
public:
    using executor_type = typename AsyncWriteStream::executor_type;

    explicit AsyncWriteQueue(AsyncWriteStream &stream)
        : stream_(stream), strand_(stream.get_executor()) {
    }

    executor_type get_executor() {
        return strand_.get_executor();
    }

    void write(std::shared_ptr<std::vector<char> > buffer) {
        boost::asio::post(
            strand_,
            [self = this->shared_from_this(), buffer = std::move(buffer)] {
                self->queue_.push_back(std::move(buffer));
                if (!self->write_in_progress_) {
                    self->write_in_progress_ = true;
                    self->do_write();
                }
            });
    }

    void cancel() {
        boost::asio::post(
            strand_,
            [self = this->shared_from_this()] {
                self->queue_.clear();
                // not setting write_in_progress_ to false here to let the current operation finish
            });
    }

private:
    void do_write() {
        auto self = this->shared_from_this();
        boost::asio::async_write(
            stream_,
            boost::asio::buffer(*queue_.front()),
            boost::asio::bind_executor(
                strand_,
                [self](boost::system::error_code ec, std::size_t) {
                    if (ec) {
                        std::cerr << "[AsyncWriteQueue] Write failed: " << ec.message() << std::endl;
                        self->queue_.clear();
                        self->write_in_progress_ = false;
                        return;
                    }

                    if (self->queue_.empty()) {
                        self->write_in_progress_ = false;
                        return;
                    }

                    self->queue_.pop_front();

                    if (!self->queue_.empty()) {
                        self->do_write();
                    } else {
                        self->write_in_progress_ = false;
                    }
                }
            )
        );
    }

    AsyncWriteStream &stream_;
    boost::asio::strand<executor_type> strand_;

    std::deque<std::shared_ptr<std::vector<char> > > queue_;
    bool write_in_progress_ = false;
};
