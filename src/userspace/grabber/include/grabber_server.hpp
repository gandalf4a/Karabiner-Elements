#pragma once

#include "constants.hpp"
#include "device_grabber.hpp"
#include "local_datagram_server.hpp"
#include "session.hpp"
#include "userspace_defs.h"
#include <vector>

class grabber_server final {
public:
  grabber_server(void) : exit_loop_(false), grabber_client_pid_monitor_(0) {
    enum {
      buffer_length = 8 * 1024 * 1024,
    };
    buffer_.resize(buffer_length);
  }

  void start(void) {
    const char* path = constants::get_grabber_socket_file_path();
    unlink(path);
    server_ = std::make_unique<local_datagram_server>(path);

    uid_t uid;
    if (session::get_current_console_user_id(uid)) {
      chown(path, uid, 0);
    }
    chmod(path, 0600);

    exit_loop_ = false;
    thread_ = std::thread([this] { this->worker(); });
  }

  void stop(void) {
    unlink(constants::get_grabber_socket_file_path());

    exit_loop_ = true;
    if (thread_.joinable()) {
      thread_.join();
    }

    server_.reset(nullptr);
    release_grabber_client_pid_monitor();
    device_grabber_.reset(nullptr);
  }

  void worker(void) {
    if (!server_) {
      return;
    }

    while (!exit_loop_) {
      boost::system::error_code ec;
      std::size_t n = server_->receive(boost::asio::buffer(buffer_), boost::posix_time::seconds(1), ec);

      if (!ec && n > 0) {
        switch (buffer_[0]) {
        case KRBN_OPERATION_TYPE_CONNECT:
          if (n != sizeof(krbn_operation_type_connect)) {
            logger::get_logger().error("invalid size for KRBN_OPERATION_TYPE_CONNECT");
          } else {
            auto p = reinterpret_cast<krbn_operation_type_connect*>(&(buffer_[0]));
            auto pid = p->console_user_server_pid;

            logger::get_logger().info("grabber_client is connected (pid:{0})", pid);

            device_grabber_.reset(nullptr);
            device_grabber_ = std::make_unique<device_grabber>();

            // monitor the last process
            release_grabber_client_pid_monitor();
            grabber_client_pid_monitor_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC,
                                                                 p->console_user_server_pid,
                                                                 DISPATCH_PROC_EXIT,
                                                                 dispatch_get_main_queue());
            if (!grabber_client_pid_monitor_) {
              logger::get_logger().error("{0}: failed to dispatch_source_create", __PRETTY_FUNCTION__);
            } else {
              dispatch_source_set_event_handler(grabber_client_pid_monitor_, ^{
                logger::get_logger().info("grabber_client is exited (pid:{0})", pid);

                device_grabber_.reset(nullptr);
                release_grabber_client_pid_monitor();
              });
              dispatch_resume(grabber_client_pid_monitor_);
            }
          }
          break;

        case KRBN_OPERATION_TYPE_DEFINE_SIMPLE_MODIFICATIONS:
          if (n < sizeof(krbn_operation_type_define_simple_modifications)) {
            logger::get_logger().error("invalid size for KRBN_OPERATION_TYPE_DEFINE_SIMPLE_MODIFICATIONS ({0})", n);
          } else {
            auto p = reinterpret_cast<krbn_operation_type_define_simple_modifications*>(&(buffer_[0]));
            if (device_grabber_) {
              device_grabber_->set_simple_modifications(p->data, p->size);
            }
          }
          break;
        }
      }
    }
  }

private:
  void release_grabber_client_pid_monitor(void) {
    if (grabber_client_pid_monitor_) {
      dispatch_source_cancel(grabber_client_pid_monitor_);
      dispatch_release(grabber_client_pid_monitor_);
      grabber_client_pid_monitor_ = 0;
    }
  }

  std::vector<uint8_t> buffer_;
  std::unique_ptr<local_datagram_server> server_;
  std::thread thread_;
  volatile bool exit_loop_;

  std::unique_ptr<device_grabber> device_grabber_;

  dispatch_source_t grabber_client_pid_monitor_;
};