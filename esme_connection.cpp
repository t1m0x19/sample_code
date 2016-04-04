/**
 * @file esme_connection.cpp
 * Implementation of esme related functionality
 * @author Timur Allamiyarov
 * @version 1.0
 */

#include <arpa/inet.h>
#include <vector>

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/date_time/posix_time/posix_time_io.hpp>
#include <boost/bind.hpp>
#include <boost/make_shared.hpp>

#include "header_parser.hpp"
#include "logger.hpp"
#include "esme_connection.hpp"
#include "memory_fence.hpp"
#include "smsc_connection.hpp"
#include "sgw_manager.hpp"

namespace async {
namespace smpp  {

/**
 * Constuctor sets connection time and session state to OPEN
 */
esme_connection::esme_connection( io_service_ptr & io_service,
                                  sgw_manager    * manager )
  : socket_(*io_service),
    service_number_( 0 ),
    throttling_threshold_(0),
    throttling_is_applicable_( false ),
    manager_(manager),
    throttling_submit_sm_counter_(0),
    submit_sm_total_number_(0),
    enquire_link_counter_(0),
    io_esme_service_ptr_( io_service ),
    throttling_check_timer_( new boost::asio::deadline_timer(*io_esme_service_ptr_) )
{
    set_connection_time();
    set_session_state( OPEN );
}

const std::string esme_connection::get_remote_ip() const
{
    boost::asio::ip::tcp::endpoint remote_ep = socket_.remote_endpoint();
    boost::asio::ip::address remote_ad = remote_ep.address();
    return remote_ad.to_string();
}

void esme_connection::set_service_number( boost::uint32_t service_number )
{
    service_number_ = service_number;
}

void esme_connection::set_throttling_threshold( boost::uint32_t threshold )
{
    throttling_threshold_ = threshold;

    /// Set throttling timer
    throttling_check_timer_->expires_from_now(
        boost::posix_time::seconds( throttling_check_interval )
    );
    throttling_check_timer_->async_wait(boost::bind(
        &esme_connection::throttling_check, this,
        throttling_check_timer_, boost::asio::placeholders::error)
    );
}

void esme_connection::throttling_check( const timer_ptr & timer,
                                        const boost::system::error_code & error )
{
    if( !error )
    {
        /// Get number of submit sm requits at the momemt
        std::size_t throttling_submit_sm_counter =
            memory_fence::load_consume<std::size_t>( &throttling_submit_sm_counter_ );

        if( throttling_submit_sm_counter > throttling_threshold_ )
        {
            /// Throttling is applicable
            memory_fence::store_release<bool>( &throttling_is_applicable_, true );
            memory_fence::store_release<std::size_t>( &throttling_submit_sm_counter_, 0 );
        }
        else
        {
            /// Throttling is not applicable
            memory_fence::store_release<bool>( &throttling_is_applicable_, false );
            memory_fence::store_release<std::size_t>( &throttling_submit_sm_counter_, 0 );
        }

        /// Set timer for new job
        timer->expires_from_now(
            boost::posix_time::seconds( throttling_check_interval )
        );
        timer->async_wait(boost::bind(
            &esme_connection::throttling_check, this,
                timer, boost::asio::placeholders::error)
        );
    }
    else
    {
        /// Report the error occured
        logger::log().error_message( boost::lexical_cast<std::string>(error) );
    }
}

void esme_connection::set_connection_time()
{
    boost::unique_lock<boost::shared_mutex> lock(connection_time_mutex_);
    connection_time_ = boost::posix_time::microsec_clock::local_time();
}

void esme_connection::set_last_activity_time()
{
    boost::unique_lock<boost::shared_mutex> lock(last_activity_mutex_);
    last_activity_time_ = boost::posix_time::microsec_clock::local_time();
}

void esme_connection::set_session_state( int state )
{
    boost::unique_lock<boost::shared_mutex> lock(session_state_mutex_);
    session_state_ = state;
}

int esme_connection::get_session_state() const
{
    boost::shared_lock<boost::shared_mutex> lock(session_state_mutex_);
    return session_state_;
}

boost::asio::ip::tcp::socket& esme_connection::socket()
{
    return socket_;
}

bool esme_connection::is_time_elapsed()
{
    /// Get current time
    boost::posix_time::ptime current_time =
        boost::posix_time::microsec_clock::local_time();

    boost::shared_lock<boost::shared_mutex> lock(last_activity_mutex_);
    boost::posix_time::time_duration elapsed_time =
        current_time - last_activity_time_;

    int elapsed_seconds = elapsed_time.seconds();

    if( elapsed_seconds > manager_->get_inactivity_period() )
    {
       return true; /// Time has expried
    }
    else
        return false;
}

/// Start the first asynchronous operation for the connection
void esme_connection::receive_packets_from_esme()
{
    memset(header_, 0, header_size);
    boost::asio::async_read(socket_,
        boost::asio::buffer(header_, header_size),
            boost::bind(&esme_connection::parse_header, shared_from_this(),
            boost::asio::placeholders::error,
            boost::asio::placeholders::bytes_transferred));
}

void esme_connection::parse_header(const boost::system::error_code& error,
                                   std::size_t bytes_transferred)
{
	if( !error )
    {
        smpp_header   hdr;
        header_parser parser(header_, header_size);

        parser >> hdr.command_length >> hdr.command_id
               >> hdr.command_status >> hdr.sequence_no;

        /// Prepare storage to get the rest of packet
        memset(data_, 0, max_length);

        /// Continue reading remaining data until EOF
        boost::asio::async_read( socket_,
            boost::asio::buffer( data_, hdr.command_length - header_size),
            boost::bind(&esme_connection::parse_body, shared_from_this(),
                boost::asio::placeholders::error,
                boost::asio::placeholders::bytes_transferred,
                hdr.command_id, hdr.command_status, hdr.sequence_no )
        );
    }
    else
    {
        handle_connection_close();
    }
}

void esme_connection::parse_body( const boost::system::error_code& error,
                                  std::size_t bytes_transferred,
                                  boost::uint32_t command_id,
                                  boost::uint32_t command_status,
                                  boost::uint32_t sequence_id  )
{
    if( !error )
    {
        switch( command_id )
        {
            case BIND_TRANS:
            {
                message_handler_ptr smpp_message =
                    boost::make_shared<message_handler>(
                        header_, data_, bytes_transferred );
                handle_bind_sm(smpp_message);
                break;
            }

            case BIND_TX:
            {
                message_handler_ptr smpp_message =
                    boost::make_shared<message_handler>(
                        header_, data_, bytes_transferred );
                handle_bind_sm(smpp_message);
                break;
            }

            case BIND_RX:
            {
                message_handler_ptr smpp_message =
                    boost::make_shared<message_handler>(
                        header_, data_, bytes_transferred );
                handle_bind_sm(smpp_message);
                break;
            }

            case SUBMIT_SM:
            {
                ++submit_sm_total_number_;

                std::size_t delta =
                    memory_fence::load_consume<std::size_t>( &throttling_submit_sm_counter_ ) + 1;
                memory_fence::store_release<std::size_t>( &throttling_submit_sm_counter_, delta );

                bool throttling_is_applicable =
                    memory_fence::load_consume<bool>( &throttling_is_applicable_ );

                if( throttling_is_applicable )
                {
                    /// Create submit_sm_resp with MESSAGE QUEUE FULL status
                    submit_sm_resp_error_ptr submit_sm_resp =
                        boost::make_shared<submit_sm_resp_error>( sequence_id );

                    submit_sm_resp->set_command_status( ESME_RMSGQFUL );
                    submit_sm_resp->create();

                    /// Send submit_sm
                    boost::asio::async_write(socket_,
                        boost::asio::buffer(submit_sm_resp->get_submit_sm_resp_butter(),
                            submit_sm_resp->get_submit_sm_resp_size()),
                            boost::bind(&esme_connection::handle_write_submit_resp,
                                shared_from_this(), submit_sm_resp,
                                boost::asio::placeholders::error)
                    );
                    /// Continue getting packets
                    set_last_activity_time();
                    receive_packets_from_esme();
                }
                else
                {
                    /// create submit_sm message
                    message_handler_ptr submit_sm =
                        boost::make_shared<message_handler>( header_, data_, bytes_transferred );
                    /// set sequence number
                    submit_sm->set_sequence_id(sequence_id);
                    /// parse new message
                    handle_submit_sm( submit_sm );
                }
                break;
            }            

            default:
            {
                receive_packets_from_esme();
                break;
            }
        }
    }
    else
    {
        handle_connection_close();
    }
}



void esme_connection::handle_bind_sm( const message_handler_ptr& smpp_msg )
{
    const std::string gateway_id = manager_->get_gateway_name();

	const char* msg = smpp_msg->get_message();

	int offset = HEADER_SIZE;
	char system_id[ SYSTEM_ID_SIZE ];
	char password[ PASSWORD_SIZE ];

	/// Get Emse ID
	memset(system_id, 0, SYSTEM_ID_SIZE);
	strncpy(system_id, msg + offset, SYSTEM_ID_SIZE);
	offset += strlen(system_id) + 1;

	/// Get password
	memset(password, 0, PASSWORD_SIZE);
	strncpy(password, msg + offset, PASSWORD_SIZE);

    /// Authentication process
	if( manager_->if_system_id_valid( system_id ) )
	{
		if( manager_->if_password_matches( password ) )
		{
            /// Autentication went well, add smpp connection into map
            manager_->add_esme_connection( system_id, password, this );
        }
		else
		{
            /// Create bind response with "Invalid password" message
            bind_resp_ptr error_response = boost::make_shared<bind_resp>();
            error_response->set_command_status(ESME_RINVPASWD);
            error_response->create_bind_pdu(gateway_id, 0);

            /// Send bind response
            boost::asio::async_write(socket_,
                boost::asio::buffer(error_response->get_bind_buffer(), error_response->get_pdu_size()),
                boost::bind(&esme_connection::handle_write_bind_error_resp, shared_from_this(),
                    error_response, boost::asio::placeholders::error));
		}
	}
	else
	{
	    /// Send Bind response with "Invalid systed id " message
	    bind_resp_ptr error_response = boost::make_shared<bind_resp>();
        error_response->set_command_status( ESME_RINVSYSID );
        error_response->create_bind_pdu( gateway_id, 0 );

        /// Send bind
        boost::asio::async_write(socket_,
            boost::asio::buffer(error_response->get_bind_buffer(), error_response->get_pdu_size()),
            boost::bind(&esme_connection::handle_write_bind_error_resp, shared_from_this(),
                error_response, boost::asio::placeholders::error));
	}
}

void esme_connection::send_bind_response()
{
    const std::string gateway_id = manager_->get_gateway_name();

    /// Set activity time
    set_last_activity_time();

    /// Session state is active
    set_session_state( ACTIVE );

    /// Create Bing response PDU
    bind_resp_ptr response = boost::make_shared<bind_resp>();
    response->create_bind_pdu( gateway_id, 0 );

    /// Send bind response
    boost::asio::async_write(socket_,
        boost::asio::buffer(response->get_bind_buffer(), response->get_pdu_size()),
        boost::bind(&esme_connection::handle_write_bind_resp, shared_from_this(),
            response, boost::asio::placeholders::error));
}

void esme_connection::send_already_in_bind_state()
{
    /// Get Gateway ID
    const std::string gateway_id = manager_->get_gateway_name();

    /// Send Bind response with "Already bound" message
    bind_resp_ptr error_response = boost::make_shared<bind_resp>();
    error_response->set_command_status(ESME_RALYBND);
    error_response->create_bind_pdu(gateway_id, 0);

    /// Send bind response with error
    boost::asio::async_write(socket_,
        boost::asio::buffer(error_response->get_bind_buffer(), error_response->get_pdu_size()),
        boost::bind(&esme_connection::handle_write_bind_error_resp, shared_from_this(),
            error_response, boost::asio::placeholders::error));
}

void esme_connection::handle_submit_sm( const message_handler_ptr& smpp_msg )
{
    /// Send packet to smsc via manager
    manager_->send_packet_to_smsc( shared_from_this(), smpp_msg, service_number_ );
    set_last_activity_time();
	receive_packets_from_esme();
}

void esme_connection::send_submit_sm_with_error( boost::uint32_t status, boost::uint32_t sequence_id )
{
    /// Create submit_sm packet
    submit_sm_resp_error_ptr submit_sm_resp_msg =
        boost::make_shared<submit_sm_resp_error>( sequence_id );

    submit_sm_resp_msg->set_command_status( status );
    submit_sm_resp_msg->create();

    /// Send it
    boost::asio::async_write(socket_,
        boost::asio::buffer(submit_sm_resp_msg->get_submit_sm_resp_butter(),
        submit_sm_resp_msg->get_submit_sm_resp_size()),
            boost::bind(&esme_connection::handle_write_submit_resp, shared_from_this(),
            submit_sm_resp_msg, boost::asio::placeholders::error));

    set_last_activity_time();
}

void esme_connection::send_packet( const message_handler_ptr & message )
{
    io_esme_service_ptr_->post( boost::bind(
        &esme_connection::handle_write, this, message )
    );
}

void esme_connection::handle_write( const message_handler_ptr & message )
{
    boost::asio::async_write(socket_,
        boost::asio::buffer(message->get_message(),
        message->get_packet_size()),
            boost::bind(&esme_connection::handle_transmission, shared_from_this(),
            message, boost::asio::placeholders::error));
}

void esme_connection::handle_transmission( const message_handler_ptr & message,
                                           const boost::system::error_code & error )
{
    if(error)
    {
        logger::log().error_message(__FILE__, __LINE__, "smsc -> esme: packet was not sent");
        handle_connection_close();
    }
}

void esme_connection::handle_connection_close()
{
	/// Response has been sent, now the socket should be closed
    close_connection();

    /// Post task to sgw manager in order to remove connection
    manager_->remove_esme_connection(this);
}


void esme_connection::handle_write_submit_resp( const submit_sm_resp_error_ptr& pdu,
						const boost::system::error_code& error )
{
    if (!error)
    {
    }
    else
    {
        /// Initiate graceful connection closure.
        handle_connection_close();
        boost::system::error_code ignored_ec;
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored_ec);
  }
}

void esme_connection::send_enquire_link(const enquire_link_ptr & enquire_link_packet )
{
    /// Send enquire_link
    boost::asio::async_write(socket_,
        boost::asio::buffer(enquire_link_packet->get_enquire_link_message_body(),
        enquire_link_packet->get_enquire_link_message_size()),
            boost::bind(&esme_connection::handle_enquire_link_sent, shared_from_this(),
            enquire_link_packet, boost::asio::placeholders::error));
}

void esme_connection::handle_enquire_link_sent(const enquire_link_ptr & enquire_link_packet,
					       const boost::system::error_code& error)
{
    if (error)
    {
        logger::log().error_message("Enquire Link packet was not sent");
        handle_connection_close();
    }
}

void esme_connection::increment_enquire_link_seq_id()
{
    ++enquire_link_counter_;
}

int esme_connection::get_enquire_link_seq_id() const
{
    return enquire_link_counter_;
}

void esme_connection::close_connection()
{
    /// Gracefully close network connection
    boost::system::error_code ignored_ec;
	socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored_ec);
	socket_.close();
}

boost::uint32_t esme_connection::get_number() const
{
    return service_number_;
}

}/// namespace smpp
}/// namespace async

