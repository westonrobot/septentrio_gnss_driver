// *****************************************************************************
//
// © Copyright 2020, Septentrio NV/SA.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//    1. Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//    2. Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//    3. Neither the name of the copyright holder nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// *****************************************************************************

#pragma once

// C++
#include <thread>

// Linux
#include <linux/serial.h>

// Boost
#include <boost/asio.hpp>

// ROSaic
#include <septentrio_gnss_driver/abstraction/typedefs.hpp>

//! Possible baudrates for the Rx
const static std::array<uint32_t, 21> baudrates = {
    1200,    2400,    4800,    9600,    19200,   38400,   57600,
    115200,  230400,  460800,  500000,  576000,  921600,  1000000,
    1152000, 1500000, 2000000, 2500000, 3000000, 3500000, 4000000};

namespace io {
    class IoBase
    {
    public:
        IoBase(ROSaicNodeBase* node) : node_(node) {}

        virtual ~IoBase();

        virtual [[nodiscard]] bool connect() = 0;

    private:
        ROSaicNodeBase* node_;
    };

    class TcpIo : public IoBase
    {
    public:
        TcpIo(ROSaicNodeBase* node, boost::asio::io_service* ioService,
              const std::string& ip, const std::string& port) :
            IoBase(node),
            ioService_(ioService), socket_(*ioService_)
        {
            boost::asio::ip::tcp::resolver resolver(*ioService_);
            boost::asio::ip::tcp::resolver::query query(ip, port);
            endpointIterator_ = resolver.resolve(query);
        }

        ~TcpIo() { socket_.close(); }

        [[nodiscard]] bool connect()
        {
            socket_.reset(new boost::asio::ip::tcp::socket(*ioService_));

            socket_.connect(*endpointIterator_);

            socket_.set_option(boost::asio::ip::tcp::no_delay(true));
        }

    private:
        boost::asio::ip::tcp::resolver::iterator endpointIterator_;
        boost::asio::io_service* ioService_;
        boost::asio::ip::tcp::socket socket_;
    };

    class SerialIo : public IoBase
    {
    public:
        SerialIo(ROSaicNodeBase* node, boost::asio::io_service* ioService,
                 std::string serialPort, uint32_t baudrate, bool flowcontrol) :
            IoBase(node),
            flowcontrol_(flowcontrol), baudrate_(baudrate), serialPort_(ioService_)
        {
        }

        ~SerialIo() { serialPort_.close(); }

        [[nodiscard]] bool connect()
        {
            if (serialPort_.is_open())
            {
                serialPort_.close();
            }

            bool opened = false;

            while (!opened)
            {
                try
                {
                    serialPort_.open(port_);
                    opened = true;
                } catch (const boost::system::system_error& err)
                {
                    node_->log(LogLevel::ERROR,
                               "SerialCoket: Could not open serial port " +
                                   std::to_string(port_) + ". Error: " + err.what() +
                                   ". Will retry every second.");

                    using namespace std::chrono_literals;
                    std::this_thread::sleep_for(1s);
                }
            }

            // No Parity, 8bits data, 1 stop Bit
            serialPort_.set_option(
                boost::asio::serial_port_base::baud_rate(baudrate_));
            serialPort_.set_option(boost::asio::serial_port_base::parity(
                boost::asio::serial_port_base::parity::none));
            serialPort_.set_option(boost::asio::serial_port_base::character_size(8));
            serialPort_.set_option(boost::asio::serial_port_base::stop_bits(
                boost::asio::serial_port_base::stop_bits::one));
            serialPort_.set_option(boost::asio::serial_port_base::flow_control(
                boost::asio::serial_port_base::flow_control::none));

            int fd = serialPort_.native_handle();
            termios tio;
            // Get terminal attribute, follows the syntax
            // int tcgetattr(int fd, struct termios *termios_p);
            tcgetattr(fd, &tio);

            // Hardware flow control settings_->.
            if (flowcontrol_)
            {
                tio.c_iflag &= ~(IXOFF | IXON);
                tio.c_cflag |= CRTSCTS;
            } else
            {
                tio.c_iflag &= ~(IXOFF | IXON);
                tio.c_cflag &= ~CRTSCTS;
            }
            // Setting serial port to "raw" mode to prevent EOF exit..
            cfmakeraw(&tio);

            // Commit settings, syntax is
            // int tcsetattr(int fd, int optional_actions, const struct termios
            // *termios_p);
            tcsetattr(fd, TCSANOW, &tio);
            // Set low latency
            struct serial_struct serialInfo;

            ioctl(fd, TIOCGSERIAL, &serialInfo);
            serialInfo.flags |= ASYNC_LOW_LATENCY;
            ioctl(fd, TIOCSSERIAL, &serialInfo);

            return setBaudrate();
        }

        [[nodiscard]] bool setBaurate()
        {
            // Setting the baudrate, incrementally..
            node_->log(LogLevel::DEBUG,
                       "Gradually increasing the baudrate to the desired value...");
            boost::asio::serial_port_base::baud_rate current_baudrate;
            node_->log(LogLevel::DEBUG, "Initiated current_baudrate object...");
            try
            {
                serialPort_.get_option(
                    current_baudrate); // Note that this sets
                                       // current_baudrate.value() often to 115200,
                                       // since by default, all Rx COM ports,
                // at least for mosaic Rxs, are set to a baudrate of 115200 baud,
                // using 8 data-bits, no parity and 1 stop-bit.
            } catch (boost::system::system_error& e)
            {

                node_->log(LogLevel::ERROR,
                           "get_option failed due to " + std::string(e.what()));
                node_->log(LogLevel::INFO, "Additional info about error is " +
                                               boost::diagnostic_information(e));
                /*
                boost::system::error_code e_loop;
                do // Caution: Might cause infinite loop..
                {
                    serialPort_.get_option(current_baudrate, e_loop);
                } while(e_loop);
                */
                return false;
            }
            // Gradually increase the baudrate to the desired value
            // The desired baudrate can be lower or larger than the
            // current baudrate; the for loop takes care of both scenarios.
            node_->log(LogLevel::DEBUG,
                       "Current baudrate is " +
                           std::to_string(current_baudrate.value()));
            for (uint8_t i = 0; i < baudrates.size(); i++)
            {
                if (current_baudrate.value() == baudrate_)
                {
                    break; // Break if the desired baudrate has been reached.
                }
                if (current_baudrate.value() >= baudrates[i] &&
                    baudrate_ > baudrates[i])
                {
                    continue;
                }
                // Increment until Baudrate[i] matches current_baudrate.
                try
                {
                    serialPort_.set_option(
                        boost::asio::serial_port_base::baud_rate(baudrates[i]));
                } catch (boost::system::system_error& e)
                {

                    node_->log(LogLevel::ERROR,
                               "set_option failed due to " + std::string(e.what()));
                    node_->log(LogLevel::INFO, "Additional info about error is " +
                                                   boost::diagnostic_information(e));
                    return false;
                }
                using namespace std::chrono_literals;
                std::this_thread::sleep_for(500ms);

                try
                {
                    serialPort_.get_option(current_baudrate);
                } catch (boost::system::system_error& e)
                {

                    node_->log(LogLevel::ERROR,
                               "get_option failed due to " + std::string(e.what()));
                    node_->log(LogLevel::INFO, "Additional info about error is " +
                                                   boost::diagnostic_information(e));
                    /*
                    boost::system::error_code e_loop;
                    do // Caution: Might cause infinite loop..
                    {
                        serialPort_.get_option(current_baudrate, e_loop);
                    } while(e_loop);
                    */
                    return false;
                }
                node_->log(LogLevel::DEBUG,
                           "Set ASIO baudrate to " +
                               std::to_string(current_baudrate.value()));
            }
            node_->log(LogLevel::INFO, "Set ASIO baudrate to " +
                                           std::to_string(current_baudrate.value()) +
                                           ", leaving InitializeSerial() method");
        }

    private:
        bool flowcontrol_;
        std::string port_;
        uint32_t baudrate_;
        boost::asio::io_service* ioService_;
        boost::asio::serial_port serialPort_;
    };
} // namespace io