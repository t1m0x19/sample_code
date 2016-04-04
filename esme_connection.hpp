#ifndef ESME_CONNECTION_HPP
#define ESME_CONNECTION_HPP

/**
 * @file esme_connection.hpp
 * @breaf This .hpp defines class for asynchronous data handling with esme
 *
 * @author Timur Allamiyarov
 * @date 2012.07.16
 * @version 0.1
 */

#include <boost/array.hpp>
#include <boost/cstdint.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/chrono/thread_clock.hpp>
#include <boost/detail/atomic_count.hpp>
#include <boost/interprocess/detail/atomic.hpp>
#include <boost/noncopyable.hpp>
#include <boost/timer.hpp>
#include <boost/thread.hpp>

#include "bind_sm_resp.hpp"
#include "enquire_link.hpp"
#include "enums.hpp"
#include "message_handler.hpp"
#include "submit_sm_resp.hpp"
#include "typedefs.hpp"

namespace sunet {
namespace smpp {

class sgw_manager;

/**
 * Class represents a single connection from a client
 */
class esme_connection
  : public boost::enable_shared_from_this<esme_connection>,
    private boost::noncopyable
{
    typedef boost::shared_ptr<bind_resp> bind_resp_ptr;
    typedef boost::shared_ptr<submit_sm_resp_error> submit_sm_resp_error_ptr;
public:
    /**
     * Creatre a connection with the given io_service
     * @param io_service reference to io_service
     * @param manager pointer to the gateway's manager
     */
    explicit esme_connection( io_service_ptr & io_service,
                              sgw_manager * manager );
    /**
     * Return socket, associated with this connection
     * @return reference to socket
     */
    boost::asio::ip::tcp::socket& socket();

    /**
     * Function to get packets from client
     */
    void receive_packets_from_esme();

    /**
     * Check elapsed time to perform keep alive check
     * @return true or false
     */
    bool is_time_elapsed();

    /**
     * Check if connection has already sent bind request
     * @return true or false
     */
    bool if_not_binded();

    /**
     * Return value of how many link packets were sent to client
     * @return number of enquire link packets
     */
    int get_enquire_link_seq_id() const;

    /**
     * Send enquire link to client
     * @return nothing
     */
    void send_enquire_link( const enquire_link_ptr & );

    /**
     * General function to send smpp packet of any kind
     * @param pointer to enquire link packet
     */
    void send_packet( const message_handler_ptr & );

    /**
     * Function to set the threshold of manimum number of packets to be
     * received per second
     * @param threshold value
     */
    void set_throttling_threshold( boost::uint32_t );

    /**
     * Function to send reponse to submit request with error
     * @param error code
     * @param sequence id of corresponding request
     */
    void send_submit_sm_with_error( boost::uint32_t, boost::uint32_t );

    /**
     * Send bind response
     */
    void send_bind_response();

    /**
     * Create and send bind reponse with "already bound" error
     */
    void send_already_in_bind_state();

    /**
     * Set short code of esme accociated with this connection
     * @param esme's short code
     */
    void set_service_number( boost::uint32_t service_number );

    /**
     * Get short code of esme
     * @return short code
     */
    boost::uint32_t get_number() const;

    /**
     * Gracely close current connection
     */
    void close_connection();

    const std::string get_remote_ip() const;

private:
    /**
     * Write time when emse connects
     */
    void set_connection_time();

    /**
     * Completion handler of a read operation
     * @param e reference to the error code of operation
     * @param bytes_transferred number of bytes sent to network
     */
    void handle_read( const boost::system::error_code & e,
                      std::size_t bytes_transferred );

    /**
     * Completion handler of a white operation
     * @param reference to the packet
     */
    void handle_write( const message_handler_ptr & );

    /**
     * Completion handler of send bind response operation
     * @param msg reference to the bind reponse packet
     * @param error reference to the error code
     */
    void handle_write_bind_resp( const bind_resp_ptr& msg,
                                 const boost::system::error_code & error );
    /**
     * Completion handler of send bind with reponse operation
     * @param msg reference of bind_resp packet with error code
     * @param error reference to the error code
     */
    void handle_write_bind_error_resp( const bind_resp_ptr & msg,
                                       const boost::system::error_code & error );
    /**
     * Completion handler of send submit sm operation
     * @param msg reference of bind_resp packet with error code
     * @param error reference to the error code
     */
    void handle_write_submit_resp( const submit_sm_resp_error_ptr & msg ,
                                   const boost::system::error_code& error );

    /**
     * Completion handler of send enquire link operation
     * @param msg reference of bind_resp packet with error code
     * @param error reference to the error code
     */
    void handle_enquire_link_sent( const enquire_link_ptr & msg,
                                   const boost::system::error_code & error );

    /**
     * Completion handler of send operation
     * @param msg reference of bind_resp packet with error code
     * @param error reference to the error code
     */
    void handle_transmission( const message_handler_ptr & msg,
                              const boost::system::error_code & error );
    /**
     * Completion handler of a parse header operation
     * @param error reference to the error code of operation
     * @param bytes_transferred number of bytes sent to network
     */
    void parse_header( const boost::system::error_code& error,
                       std::size_t bytes_transferred );

    /**
     * Completion handler of parse body operation
     * @param e reference to the error code of operation
     * @param bytes_transferred number of bytes sent to network
     */
    void parse_body( const boost::system::error_code&, std::size_t,
                     boost::uint32_t, boost::uint32_t, boost::uint32_t );
    /**
     * Process bind_sm packet
     * @param reference to the bind_sm packet
     */
    void handle_bind_sm( const message_handler_ptr & msg );

    /**
     * Process submit_sm packet
     * @param reference to the submit_sm packet
     */
    void handle_submit_sm( const message_handler_ptr & msg );

    /**
     * Completion handler for close operation
     */
    void handle_connection_close();

    /**
     * Assign time of last activity time
     */
    void set_last_activity_time();

    /**
     * Set current state of connection
     * @param current sesion's state
     */
    void set_session_state( int state );

    /**
     * Get current state
     * @return state_
     */
    int  get_session_state() const;

    /**
     * Increase value fo the enquire link counter
     */
    void increment_enquire_link_seq_id();

    /**
     * Completion handler of a throttling techinique
     * @param timer reference to the timer set for this operation
     * @param error reference to the system error occured
    */
    void throttling_check( const timer_ptr & timer,
                           const boost::system::error_code & error );

    boost::asio::ip::tcp::socket socket_;               /**< socket, associated with the given connection */
    boost::posix_time::ptime     connection_time_;      /**< Connection timestamp */
    mutable boost::shared_mutex connection_time_mutex_; /**< mutex to prevent data race on connection_time */

    boost::posix_time::ptime last_activity_time_;     /**< timstamp of last activity */
    mutable boost::shared_mutex last_activity_mutex_; /**< mutex to prevent data race on activityu time */

    enum { header_size = 16, max_length = 65536 };
    boost::uint8_t header_[ header_size ];        /**< header of smpp message */
    boost::uint8_t data_[max_length];             /**< the body of the smpp message */

    boost::uint32_t service_number_;       /**< service number associated with connection */
    boost::uint32_t throttling_threshold_; /**< threshold to be used to prevent flood */
    bool throttling_is_applicable_;     /**< flag check if throttling must take place */
    sgw_manager * manager_;             /**< pointer to the manager module */

    std::size_t throttling_submit_sm_counter_;           /**< number of requests sent per second */

    boost::detail::atomic_count submit_sm_total_number_; /**< total number requests */
    boost::detail::atomic_count enquire_link_counter_;   /**< total number of enquire link packets */

    io_service_ptr & io_esme_service_ptr_; /**< io_service object */
    timer_ptr throttling_check_timer_;     /**< timer to perform throttling */

    enum { throttling_check_interval = 1 };

    int session_state_;                                /**< current state of connection */
    mutable boost::shared_mutex session_state_mutex_;  /**< mutex to prevent data race on connection state */
};

typedef boost::shared_ptr<esme_connection> esme_connection_ptr;

} // namespace smpp
} // namespace sunet

#endif /* ESME_CONNECTION_HPP */
