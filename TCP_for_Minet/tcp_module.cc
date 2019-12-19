// By Brian Popeck and Nathan Hall
// See README for solution specifics



#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>


#include <iostream>

#include "Minet.h"

#include "tcpstate.h"   // note that tcpstate.h is not included in Minet.h in order to avoid circular imports

// determines whether or not the module will refuse to select initial sequence numbers for MSL seconds in order to not confuse new connections with connections which existed prior to module startup 
// see pp. 28-30 of the RFC
#define TCP_QUIET_TIME_ACTIVE 0

// the retransmission timer is initialized to 2*3=6 seconds per p. 331 of Illustrated pdf
// note that this value is multiplied by BETA to set the first timeout duration
#define INITIAL_TCP_TRANSMISSION_TIMER  3

// constants used to calculate retransmission timeout length
// see p. 41 of the RFC, we chose the sample values given there
#define ALPHA 0.9
#define BETA 2
#define TRANSMISSION_TIMER_UPPER_BOUND 60   // in seconds
#define TRANSMISSION_TIMER_LOWER_BOUND 1    // in seconds

using namespace std;

// State class for TCP connections that adds precedence and RTT measuring (for timeouts)
// note for lack of Minet documentation to the contrary, we assume there is only one security level
class MyTCPState: public TCPState {
    public:
        Time packet_last_transmitted;  // timestamp of when the last packet was sent (not including ACKs without data)
                                        // used to calculate RTT when corresponding ACK is received
        Time srtt;  // smoothed round trip time

        unsigned char precedence;   // precedence as communicated by the IP header

        // extend parent constructor to initialize new variables
        MyTCPState() : TCPState()
        {
            packet_last_transmitted = 0;
            srtt = INITIAL_TCP_TRANSMISSION_TIMER;
            precedence = 0;
        }

        // extend parent constructor to initialize new variables
        MyTCPState(unsigned int initialSequenceNum, unsigned int state, unsigned int timertries) : TCPState(initialSequenceNum, state, timertries)
        {
            packet_last_transmitted = 0;
            srtt = INITIAL_TCP_TRANSMISSION_TIMER;
            precedence = 0;
        }

        // slight modification of the equivalent function in TCPState -- fixes the off-by- one error that deletes one more byte than it should from SendBuffer
        // expects an ACK number from a TCP header, not the number of the last byte acked (differ by one)
        // returns true if 0 or more bytes were successfully deleted from the front of Send Buffer
        bool SetLastAcked(unsigned int newack)
        {
          if(last_acked <= last_sent) {
            if(newack > last_acked && newack <= last_sent + 1) {
              //Delete the front of the buffer
              SendBuffer.Erase(0, newack - last_acked - 1);
              last_acked = newack - 1;
              return true;
            } else {
              return false;
            }
          } else if(newack > last_acked && newack > last_sent) {    // overflow case 1: only last_sent has overflowed
            //Delete the front of the buffer
            SendBuffer.Erase(0, newack - last_acked - 1);
            last_acked = newack - 1;
            return true;
          } else if(newack < last_acked && newack <= last_sent + 1) {   // overflow case 2: both newack and last_sent have overflowed
            //Delete the front of the buffer
            SendBuffer.Erase(0, (SEQ_LENGTH_MASK - last_acked + 1) + newack - 1);
            last_acked = newack - 1;
            return true;
          } else
            return false;
        }
};


// for receiving packets
void ExtractHeadersFromPacket(Packet &p, IPHeader &iph, TCPHeader &tcph);
void ConfigureIncomingConnection(IPHeader &iph, TCPHeader &tcph, Connection &connection);
size_t GetPayloadSize(IPHeader &iph, TCPHeader &tcph);
bool ReceivePayload(Buffer &payload, IPHeader &iph, TCPHeader &tcph, ConnectionToStateMapping<MyTCPState> &conn_to_state_map
    , deque<SockRequestResponse> &sock_req_list, MinetHandle sock);
bool ProcessAck(unsigned int &partner_ack_num, unsigned char &flags, ConnectionToStateMapping<MyTCPState> &conn_to_state_map, Time &time_at_event, bool b_syn_or_fin);

// for testing sequence/ack numbers
bool IsExpectedAckNum(unsigned int &partner_ack_num, MyTCPState &state);
bool IsSentDataAcked(unsigned int &partner_ack_num, MyTCPState &state);
bool IsDuplicateAck(unsigned int &partner_ack_num, MyTCPState &state);
int GetBytesAcked(unsigned int &partner_ack_num, MyTCPState &state);
bool IsAcceptableSegment(unsigned int &partner_seq_num, size_t &payload_size, MyTCPState &state);

// for constructing/sending packets
void InitializeSequenceNumber(unsigned int &sequence_num);
Buffer GetPayloadToSend(MyTCPState &state);
Packet BuildPacket(ConnectionToStateMapping<MyTCPState> &conn_to_state_map, unsigned char &flags, bool send_data_if_possible);
int SendPacketWrapper(const MinetHandle &handle, const Packet &p);

// for timeout management
void AdvanceTimeouts(ConnectionList<MyTCPState> &clist, double &elapsed_time);
void UpdateEventLoopTimeout(ConnectionList<MyTCPState> &clist, double &event_loop_timeout, Time &time_before_block);
void restartTIME_WAITtimer(ConnectionToStateMapping<MyTCPState> &conn_to_state_map);

// for sending socket messages
void zeroByteTCPWrite(MinetHandle sock, deque<SockRequestResponse> &sock_req_list, Connection &connection, int error);
void SendTCPWrite(MinetHandle sock, ConnectionToStateMapping<MyTCPState> &conn_to_state_map, deque<SockRequestResponse> &sock_req_list);
void SendTCPClose(MinetHandle sock, deque<SockRequestResponse> &sock_req_list, Connection &connection, int error);
void SendSocketMessageWrapper(MinetHandle sock, SockRequestResponse &message);

// for opening connections
void activeOpen(MinetHandle mux, MinetHandle sock, ConnectionList<MyTCPState> &clist, Connection &connection);
void passiveOpen(MinetHandle mux, MinetHandle sock, ConnectionList<MyTCPState> &clist, Connection &connection);

// for sending specific packet types
void sendSYN(MinetHandle mux, ConnectionToStateMapping<MyTCPState> &conn_to_state_map, bool send_data_if_possible);
void sendSYN_ACK(MinetHandle mux, ConnectionToStateMapping<MyTCPState> &conn_to_state_map, bool send_data_if_possible);
void sendACK(MinetHandle mux, ConnectionToStateMapping<MyTCPState> &conn_to_state_map, bool send_data_if_possible);
void sendFIN_ACK(MinetHandle mux, ConnectionToStateMapping<MyTCPState> &conn_to_state_map);
void sendRST(MinetHandle mux, ConnectionToStateMapping<MyTCPState> &conn_to_state_map);
void sendRSTWithSeq(MinetHandle mux, ConnectionToStateMapping<MyTCPState> &conn_to_state_map, unsigned int &sequence_num);
void sendRST_ACK(MinetHandle mux, ConnectionToStateMapping<MyTCPState> &conn_to_state_map);






int main(int argc, char * argv[]) {
    MinetHandle mux;
    MinetHandle sock;
    
    ConnectionList<MyTCPState> clist;
    deque<SockRequestResponse> sock_req_list;  // list of requests made from TCP module to the socket, the head is the oldest request
                                                // expected to hold WRITE and CLOSE requests only (TCP STATUS requests do not expect a response)

    MinetInit(MINET_TCP_MODULE);

    mux = MinetIsModuleInConfig(MINET_IP_MUX) ?  
	MinetConnect(MINET_IP_MUX) : 
	MINET_NOHANDLE;
    
    sock = MinetIsModuleInConfig(MINET_SOCK_MODULE) ? 
	MinetAccept(MINET_SOCK_MODULE) : 
	MINET_NOHANDLE;

    if ( (mux == MINET_NOHANDLE) && 
	 (MinetIsModuleInConfig(MINET_IP_MUX)) ) {

	MinetSendToMonitor(MinetMonitoringEvent("Can't connect to ip_mux"));

	return -1;
    }

    if ( (sock == MINET_NOHANDLE) && 
	 (MinetIsModuleInConfig(MINET_SOCK_MODULE)) ) {

	MinetSendToMonitor(MinetMonitoringEvent("Can't accept from sock_module"));

	return -1;
    }

    MinetSendToMonitor(MinetMonitoringEvent("tcp_module STUB VERSION handling tcp traffic........"));

    MinetEvent event;
    double event_loop_timeout = 1;

    if (TCP_QUIET_TIME_ACTIVE) {    // wait for all packets sent before module launched to be drained from the network
        sleep(MSL_TIME_SECS);
    }

    // measure the time between events in order to update timers appropriately
    Time time_before_block; // initializes to the current time
    Time time_at_event;     // intializes to the current time

    while (MinetGetNextEvent(event, event_loop_timeout) == 0) {

    if ((event.eventtype == MinetEvent::Dataflow) && 
	    (event.direction == MinetEvent::IN)) {
	
        time_at_event = Time(); // mark the time at which the event occurred

	    // advance all existing active timers the appropriate amount
        double elapsed_time = (double) time_at_event - (double) time_before_block;
        AdvanceTimeouts(clist, elapsed_time);

        if (event.handle == mux) {  // a packet has arrived
            // receive the packet
            Packet p;
            MinetReceive(mux, p);
            
            // extract the headers
            IPHeader iph;
            TCPHeader tcph;
            ExtractHeadersFromPacket(p, iph, tcph);

            // ignore corrupt packets
            if (!tcph.IsCorrectChecksum(p)) {
                UpdateEventLoopTimeout(clist, event_loop_timeout, time_before_block);   // configure the next timeout
                continue;   // block
            }

            // determine the connection the incoming packet is associated with
            Connection connection;
            ConfigureIncomingConnection(iph, tcph, connection);
            
            // get the flags set on the incoming TCP header
            unsigned char partner_flags;
            tcph.GetFlags(partner_flags);

            // get the partner sequence number and partner ACK number
            unsigned int partner_seq_num;
            tcph.GetSeqNum(partner_seq_num);
            unsigned int partner_ack_num;
            tcph.GetAckNum(partner_ack_num);

            // get the partner window size
            unsigned short partner_window_size;
            tcph.GetWinSize(partner_window_size);

            // get the partner precedence
            unsigned char partner_precedence;
            iph.GetTOS(partner_precedence);
            partner_precedence >>= 5;   // precedence is the leftmost 3 bits of TOS 


            // look for an existing connection matching the one just received
            ConnectionList<MyTCPState>::iterator conn_to_state_map = clist.FindMatching(connection);
            if (conn_to_state_map != clist.end()) { // match found
                switch (conn_to_state_map->state.stateOfcnx)
                {
                    case CLOSED:
                        break;  // should never be reached, closed connections are simply deleted from the list
                    case LISTEN:
                        // pp. 65-66 of the RFC
                        {
                            if (IS_RST(partner_flags)) {    // first check for an RST
                                ;   // ignore RST
                            } else if (IS_ACK(partner_flags)) { // second check for an ACK
                                conn_to_state_map->connection = connection; // set the partner side of the connection
                                sendRSTWithSeq(mux, *conn_to_state_map, partner_ack_num);
                            } else if (IS_SYN(partner_flags)) { // third check for a SYN
                                // create a new TCP State object
                                unsigned int sequence_num;
                                InitializeSequenceNumber(sequence_num);

                                // timertries will be zero (have not retransmitted on timeout yet)
                                MyTCPState state = MyTCPState(sequence_num - 1, SYN_SENT1, 0);  // sets last_acked == last_sent == sequence_num - 1
                                state.N = TCP_MAXIMUM_SEGMENT_SIZE; // stop-and-wait protocol means only one packet allowed in flight
                                
                                // configure state based on partner state
                                state.SetLastRecvd(partner_seq_num);
                                state.SetSendRwnd(partner_window_size);
                                state.precedence = partner_precedence;

                                // update the connection to state map object
                                conn_to_state_map->connection = connection;  // populates dest IP address and port number
                                conn_to_state_map->state = state;

                                // receive data if any was sent
                                Buffer payload = p.GetPayload();
                                ReceivePayload(payload, iph, tcph, *conn_to_state_map, sock_req_list, sock);
                                
                                sendSYN_ACK(mux, *conn_to_state_map, false);   // don't send any data
                            } else {    // fourth other text or control
                                ;   // drop the packet
                            }

                            break;
                        }
                    case SYN_SENT:
                        // pp. 66-68 of the RFC
                        {
                            // first check the ACK bit
                            bool acceptable_ack = false;
                            if (IS_ACK(partner_flags)) {
                                acceptable_ack = IsExpectedAckNum(partner_ack_num, conn_to_state_map->state);
                                if (acceptable_ack) {
                                    ProcessAck(partner_ack_num, partner_flags, *conn_to_state_map, time_at_event, true);    // ACK of SYN
                                } else {  // the ack number is invalid
                                    if (!IS_RST(partner_flags)) {
                                        conn_to_state_map->connection = connection; // set the partner side of the connection
                                        sendRSTWithSeq(mux, *conn_to_state_map, partner_ack_num);
                                    }

                                    break;  // drop the packet
                                }
                            }

                            // second check the RST bit
                            if (IS_RST(partner_flags)) {
                                if (acceptable_ack) {
                                    clist.erase(conn_to_state_map);   // enter CLOSED state
                                    zeroByteTCPWrite(sock, sock_req_list, conn_to_state_map->connection, ECONN_FAILED); // tell the user that the connection failed to be established
                                }

                                break;  // drop the segment and return
                            }

                            // third check the precedence
                            if (IS_ACK(partner_flags)) {
                                if (partner_precedence != conn_to_state_map->state.precedence) {
                                    sendRSTWithSeq(mux, *conn_to_state_map, partner_ack_num);
                                }
                            } else {    // not an ACK
                                conn_to_state_map->state.precedence = partner_precedence;   // match the precedence of partner
                            }

                            // fourth check the SYN bit
                            if (IS_SYN(partner_flags)) {
                                if (acceptable_ack) {   // our SYN has been ACKed and we already processed the ACK
                                    conn_to_state_map->state.stateOfcnx = ESTABLISHED;
                                    zeroByteTCPWrite(sock, sock_req_list, conn_to_state_map->connection, EOK);    // tell the user the connection is established

                                    // set local state based on partner state
                                    conn_to_state_map->state.SetLastRecvd(partner_seq_num);
                                    conn_to_state_map->state.SetSendRwnd(partner_window_size);

                                    // receive data if any was sent
                                    Buffer payload = p.GetPayload();
                                    ReceivePayload(payload, iph, tcph, *conn_to_state_map, sock_req_list, sock);

                                    sendACK(mux, *conn_to_state_map, true);    // send data if possible
                                } else {    // received a SYN only, simultaneous open
                                    ;   // simultaneous opens are not supported
                                }

                                break;  // return
                            }

                            // fifth, if neither of the SYN or RST bits is set
                            break;  // drop the packet
                        }
                    case SYN_SENT1: // extra state not in the RFC -- represents having received and sent a SYN before connection established (the sent SYN is not yet ACKed)
                    case SYN_RCVD:
                    case ESTABLISHED:
                    case FIN_WAIT1:
                    case FIN_WAIT2:
                    case CLOSE_WAIT:
                    case CLOSING:
                    case LAST_ACK:
                    case TIME_WAIT:
                    case SEND_DATA: // extra state not in the RFC -- represents having the user requested a close on an ESTABLISHED connection (should send remaining data before closing the connection)
                        // pp. 69-76
                        {
                            // first check the sequence number
                            size_t payload_size = GetPayloadSize(iph, tcph);
                            if (!IsAcceptableSegment(partner_seq_num, payload_size, conn_to_state_map->state)) {    // incoming packet unacceptable
                                if (IS_RST(partner_flags)) {
                                    break;  // drop the segment and return
                                } else {    // not a RST
                                    // received an unacceptable segment, send an ACK
                                    sendACK(mux, *conn_to_state_map, false);    // don't send any data
                                    break;  // drop the segment and return
                                }
                            }

                            // second check the RST bit
                            if (IS_RST(partner_flags)) {
                                bool b_return = false;
                                switch (conn_to_state_map->state.stateOfcnx)
                                {
                                    case SYN_RCVD:
                                    case SYN_SENT1:
                                        // the connection was initiated with a passive open
                                        {
                                            // return the connection to the listen state and return (see p. 70 of the RFC)
                                            conn_to_state_map->bTmrActive = false;
                                            conn_to_state_map->state = MyTCPState(0, LISTEN, 0);   // reset the state, also removes all segments on the retransmission queue
                                            // reset connection to listen for any incoming connection
                                            connection.dest = IP_ADDRESS_ANY;
                                            connection.destport = PORT_ANY;
                                            b_return = true;  // return

                                            break;
                                        }
                                    case ESTABLISHED:
                                    case SEND_DATA:
                                    case FIN_WAIT1:
                                    case FIN_WAIT2:
                                    case CLOSE_WAIT:
                                        // the Minet TCP/IP stack document does not describe sending "reset" responses or "connection reset" signals to sock_module
                                        clist.erase(conn_to_state_map);   // close the connection
                                        break;
                                    case CLOSING:
                                    case LAST_ACK:
                                    case TIME_WAIT:
                                        clist.erase(conn_to_state_map);   // close the connection
                                        break;
                                }

                                if (b_return) {
                                    break;  // return
                                }
                            }

                            // third check the precedence -- p. 71 of the RFC
                            switch (conn_to_state_map->state.stateOfcnx)
                            {
                                case SYN_RCVD:
                                case SYN_SENT1:
                                    if (conn_to_state_map->state.precedence != partner_precedence) {
                                        // let user know connection failed to be established
                                        zeroByteTCPWrite(sock, sock_req_list, conn_to_state_map->connection, ECONN_FAILED);

                                        sendRSTWithSeq(mux, *conn_to_state_map, partner_ack_num);
                                    }

                                    break;
                                case ESTABLISHED:
                                    if (conn_to_state_map->state.precedence != partner_precedence) {
                                        // let the user know connection is being reset
                                        SendTCPClose(sock, sock_req_list, conn_to_state_map->connection, EOK);

                                        // send a RST
                                        sendRSTWithSeq(mux, *conn_to_state_map, partner_ack_num);
                                    }

                                    break;
                            }

                            // fourth, check the SYN bit
                            // SYN in window is in error in all the current possible states
                            if (IS_SYN(partner_flags)) {
                                conn_to_state_map->connection = connection; // make sure the connection is bound on the partner side
                                sendRST(mux, *conn_to_state_map);
                                clist.erase(conn_to_state_map);   // close the connection
                                if (conn_to_state_map->state.stateOfcnx == SYN_SENT1) { // the connection was never established
                                    zeroByteTCPWrite(sock, sock_req_list, conn_to_state_map->connection, ECONN_FAILED); // let the user know connection failed to be established
                                }
                            }
                            
                            // fifth check the ACK field (see p. 72 of the RFC)
                            if (IS_ACK(partner_flags)) {
                                bool b_return = false;
                                switch (conn_to_state_map->state.stateOfcnx)
                                {
                                    case SYN_RCVD:
                                        break;  // don't expect to see an ACK since we advance directly to SYN_SENT1 after sending a SYN
                                    case SYN_SENT1:
                                        // behavior of SYN_RCVD in RFC
                                        {
                                            if (IsExpectedAckNum(partner_ack_num, conn_to_state_map->state)) { // the SYN packet we sent was ACKed
                                                conn_to_state_map->state.stateOfcnx = ESTABLISHED;   // advance to ESTABLISHED state and continue processing
                                                zeroByteTCPWrite(sock, sock_req_list, conn_to_state_map->connection, EOK);    // tell the user the connection is established

                                                ProcessAck(partner_ack_num, partner_flags, *conn_to_state_map, time_at_event, true);    // ACK of SYN
                                            } else {    // the acknowledgment is not acceptable
                                                conn_to_state_map->connection = connection; // bind the connection on the partner side
                                                sendRSTWithSeq(mux, *conn_to_state_map, partner_ack_num);
                                            }

                                            break;
                                        }
                                    case ESTABLISHED:
                                    case SEND_DATA:
                                    case FIN_WAIT1:
                                    case FIN_WAIT2:
                                    case CLOSE_WAIT:
                                        {
                                            // remove the acknowedged sent data from Send Buffer
                                            if (IsSentDataAcked(partner_ack_num, conn_to_state_map->state)) {
                                                // update the window size; note that in stop-and-wait, the partner window size is never updated using old segments
                                                conn_to_state_map->state.SetSendRwnd(partner_window_size);

                                                if (conn_to_state_map->state.stateOfcnx == FIN_WAIT1) {  // our FIN was ACKed
                                                    conn_to_state_map->state.stateOfcnx = FIN_WAIT2; // enter FIN_WAIT2 and continue processing
                                                }
                                            } else if (IsDuplicateAck(partner_ack_num, conn_to_state_map->state)) { // duplicate ACK
                                                ;   // ignore duplicate ACKs
                                            } else {    // packet ACKs data which has not been sent yet
                                                sendACK(mux, *conn_to_state_map, false);   // don't send data
                                                b_return = true;  // drop the segment and return
                                            }

                                            if (conn_to_state_map->state.stateOfcnx == CLOSING) {
                                                if (IsSentDataAcked(partner_ack_num, conn_to_state_map->state)) { // our FIN was ACKed
                                                    conn_to_state_map->state.stateOfcnx = TIME_WAIT;
                                                } else {    // our FIN was not ACKed
                                                    b_return = true;    // ignore the segment
                                                }
                                            }

                                            // process the ACK -- only in FIN_WAIT1 we have an ACK for a FIN
                                            if (conn_to_state_map->state.stateOfcnx == FIN_WAIT1) {  // ACK of FIN
                                                ProcessAck(partner_ack_num, partner_flags, *conn_to_state_map, time_at_event, true);
                                            } else {    // ACK of neither FIN nor SYN
                                                ProcessAck(partner_ack_num, partner_flags, *conn_to_state_map, time_at_event, false);
                                            }

                                            break;
                                        }
                                    case LAST_ACK:
                                        // the only thing that can arrive is an ACK of our FIN
                                        {
                                            if (IsSentDataAcked(partner_ack_num, conn_to_state_map->state)) { // Our FIN was ACKed
                                                ProcessAck(partner_ack_num, partner_flags, *conn_to_state_map, time_at_event, true);    // ACK of FIN
                                                clist.erase(conn_to_state_map);   // close the connection
                                                b_return = true;    // return
                                            }

                                            break;
                                        }
                                    case TIME_WAIT:
                                        // the only thing that can arrive is a retransmission of a FIN
                                        {
                                            if (IS_FIN(partner_flags)) {
                                                sendACK(mux, *conn_to_state_map, false);   // don't send any data, all data should have been sent since a FIN has already been sent
                                                restartTIME_WAITtimer(*conn_to_state_map);
                                            }

                                            break;
                                        }
                                }

                                if (b_return) {
                                    break;  // return
                                }
                            } else {    // the packet did not set the ACK bit -- TCP segments in an established connection should always be ACKing data
                                break;  // drop the segment and return
                            }

                            // seventh, process the segment text
                            if (conn_to_state_map->state.stateOfcnx == ESTABLISHED || conn_to_state_map->state.stateOfcnx == SEND_DATA
                                || conn_to_state_map->state.stateOfcnx == FIN_WAIT1 || conn_to_state_map->state.stateOfcnx == FIN_WAIT2) // other states should not receive data since a FIN has already been sent
                            {
                                // receive data
                                Buffer payload = p.GetPayload();
                                ReceivePayload(payload, iph, tcph, *conn_to_state_map, sock_req_list, sock);

                                // special case where SEND_DATA has run out of data to send
                                if (conn_to_state_map->state.stateOfcnx == SEND_DATA && conn_to_state_map->state.SendBuffer.GetSize() == 0) { // sent all data, user wants to close
                                    sendFIN_ACK(mux, *conn_to_state_map);
                                    conn_to_state_map->state.stateOfcnx = FIN_WAIT1;
                                    break;  // done with this segment

                                } 

                                bool send_data = (conn_to_state_map->state.stateOfcnx == ESTABLISHED || conn_to_state_map->state.stateOfcnx == SEND_DATA)
                                    && conn_to_state_map->state.SendBuffer.GetSize() > 0;

                                bool received_data = GetPayloadSize(iph, tcph) > 0; // there was some user data in the payload
                                                                                    // even if we could not receive this data successfully, should send an ACK to partner to let them know

                                if (send_data || received_data) {   // we need to send an ACK
                                    sendACK(mux, *conn_to_state_map, true);    // send data if possible
                                } 
                            }

                            // eighth, check the FIN bit
                            if (IS_FIN(partner_flags)) {
                                zeroByteTCPWrite(sock, sock_req_list, conn_to_state_map->connection, EOK);

                                SendTCPClose(sock, sock_req_list, conn_to_state_map->connection, EOK);

                                // advance last received by 1 to account for the FIN
                                conn_to_state_map->state.SetLastRecvd(conn_to_state_map->state.GetLastRecvd() + 1);

                                // send an acknowldgment for the FIN
                                sendACK(mux, *conn_to_state_map, false);   // don't send any data

                                switch (conn_to_state_map->state.stateOfcnx)
                                {
                                    case SYN_RCVD:
                                    case SYN_SENT1:
                                    case ESTABLISHED:
                                    case SEND_DATA:
                                        conn_to_state_map->state.stateOfcnx = CLOSE_WAIT;
                                        break;
                                    case FIN_WAIT1:
                                        {
                                            if (IsSentDataAcked(partner_ack_num, conn_to_state_map->state)) { // our FIN was ACKed in this segment
                                                conn_to_state_map->state.stateOfcnx = TIME_WAIT;
                                                restartTIME_WAITtimer(*conn_to_state_map);
                                            } else {    // our FIN has not yet been ACKed
                                                conn_to_state_map->state.stateOfcnx = CLOSING;
                                            }

                                            break;
                                        }
                                    case FIN_WAIT2:
                                        {
                                            // both sides have sent FINs, so connection is now closed
                                            // need to tell the user this now because they initiated the close (partner could have sent data after user sent CLOSE)
                                            SockRequestResponse reply = SockRequestResponse(STATUS, conn_to_state_map->connection, Buffer(), 0, EOK);
                                            SendSocketMessageWrapper(sock, reply);

                                            conn_to_state_map->state.stateOfcnx = TIME_WAIT;
                                            restartTIME_WAITtimer(*conn_to_state_map);

                                            break;
                                        }
                                    case CLOSE_WAIT:
                                    case CLOSING:
                                    case LAST_ACK:
                                        break;  // remain in the same state
                                    case TIME_WAIT:
                                        restartTIME_WAITtimer(*conn_to_state_map);
                                        break;
                                }
                            }

                            break;
                        }
                    default:
                        break;
                }

            } else {    // no match found -- that means this connection is CLOSED (p. 65 of the RFC)
                if (!IS_RST(partner_flags)) {
                    // send a RST in response 
                    ConnectionToStateMapping<MyTCPState> conn_to_state_map;
                    conn_to_state_map.connection = connection;
                    MyTCPState state = MyTCPState(-1, CLOSED, 0);    // set sequence number to zero
                    conn_to_state_map.state = state;

                    if (IS_ACK(partner_flags)) {
                        sendRSTWithSeq(mux, conn_to_state_map, partner_ack_num);
                    } else {    // not an ACK
                        // ACK the partner's sequence number
                        unsigned int ack_num = partner_seq_num + GetPayloadSize(iph, tcph); // ACK partner sequence number plus number of bytes in segment
                        conn_to_state_map.state.SetLastRecvd(ack_num - 1); // sets the next ACK number to be ack_num

                        sendRST_ACK(mux, conn_to_state_map);
                    }
                }
            }
	    }  // end mux handling

	    if (event.handle == sock) {    // socket request or response has arrived
            // receive the SockRequestResponse
            SockRequestResponse req;
            MinetReceive(sock, req);

            // see sockint.h for the enum of types and fields of SockRequestResponse struct
            // Refer to pp. 15-16 of the Minet TCP/IP stack handout to see what the socket expects in return for each type
            switch(req.type) {  
                case CONNECT:   // the connection should be fully bound
                    activeOpen(mux, sock, clist, req.connection);
                    break;
                case ACCEPT:    // the connection should be fully bound on the local side and unbound on the remote side
                    passiveOpen(mux, sock, clist, req.connection);
                    break;
                case WRITE:
                    {
                        // look for the matching connection
                        ConnectionList<MyTCPState>::iterator conn_to_state_map = clist.FindMatching(req.connection);
                        if (conn_to_state_map != clist.end()) {   // found a matching connection
                            switch (conn_to_state_map->state.stateOfcnx)
                            {
                                case LISTEN:
                                    // writing data to a passive open is not supported
                                    {
                                        SockRequestResponse reply = SockRequestResponse(STATUS, req.connection, Buffer(), 0, ENOT_SUPPORTED);   // no data
                                        SendSocketMessageWrapper(sock, reply);
                                    }

                                    break;
                                case SYN_SENT:
                                case SYN_RCVD:
                                case SYN_SENT1:
                                case ESTABLISHED:
                                case CLOSE_WAIT:
                                    {
                                        unsigned bytes_queued;
                                        unsigned int send_buffer_space = conn_to_state_map->state.TCP_BUFFER_SIZE - conn_to_state_map->state.SendBuffer.GetSize();
                                        // write as much data as possible to send buffer
                                        if (req.data.GetSize() <= send_buffer_space) {  // can queue all of the data to send
                                            conn_to_state_map->state.SendBuffer.AddBack(req.data);
                                            bytes_queued = req.data.GetSize();
                                        } else {    // can only queue a subset of data to send
                                            // fill the remainder of the send buffer
                                            conn_to_state_map->state.SendBuffer.AddBack(req.data.ExtractFront(send_buffer_space));
                                            bytes_queued = send_buffer_space;
                                        }

                                        // send a response
                                        SockRequestResponse reply = SockRequestResponse(STATUS, req.connection, Buffer(), bytes_queued, 0);
                                        if (bytes_queued > 0 || req.data.GetSize() == 0) {   // some bytes were successfully queued or there was no data to queue
                                            reply.error = EOK;  // success
                                        } else {    // there was no room in the Send Buffer to store any bytes
                                            reply.error = EBUF_SPACE;
                                        }
                                        SendSocketMessageWrapper(sock, reply);

                                        bool send_packet = (conn_to_state_map->state.stateOfcnx == ESTABLISHED || conn_to_state_map->state.stateOfcnx == CLOSE_WAIT)    // connection has been established and no FIN sent yet
                                            && conn_to_state_map->state.GetLastSent() == conn_to_state_map->state.GetLastAcked()  // no outstanding unacked data
                                            && conn_to_state_map->state.SendBuffer.GetSize() > 0 // there is queued data to send
                                            && conn_to_state_map->state.rwnd > 0; // partner can accept at least one byte of data
                                        if (send_packet) {
                                            sendACK(mux, *conn_to_state_map, true);   // send data
                                        }

                                        break;
                                    }
                                case SEND_DATA:
                                case FIN_WAIT1:
                                case FIN_WAIT2:
                                case LAST_ACK:
                                case CLOSING:
                                case TIME_WAIT:
                                    {
                                        // the connection is in the process of closing (is no longer sending data)
                                        SockRequestResponse reply = SockRequestResponse(STATUS, req.connection, Buffer(), 0, EINVALID_OP);
                                        SendSocketMessageWrapper(sock, reply);

                                        break;
                                    }
                            }
                        } else {    // did not find a matching connection
                            // send a response indicating error
                            SockRequestResponse reply = SockRequestResponse(STATUS, req.connection, Buffer(), 0, ENOMATCH);
                            SendSocketMessageWrapper(sock, reply);
                        }

                        break;
                    }
                case FORWARD:
                    // ignore this message and return a zero-error status
                    {
                        // indicate that there is no error 
                        SockRequestResponse reply = SockRequestResponse(STATUS, req.connection, Buffer(), 0, EOK);
                        SendSocketMessageWrapper(sock, reply);
                    }
                    break;
                case CLOSE:
                    {
                        // look for the matching connection
                        ConnectionList<MyTCPState>::iterator conn_to_state_map = clist.FindMatching(req.connection);
                        if (conn_to_state_map != clist.end()) {   // found a matching connection (the connection is not already closed)
                            switch (conn_to_state_map->state.stateOfcnx)
                            {
                                case SYN_RCVD:
                                case SYN_SENT:
                                case SYN_SENT1:
                                    // no connection was established
                                    clist.erase(conn_to_state_map);   // delete the connection
                                    break;
                                case ESTABLISHED:
                                    // if there is no remaining data to send, send the FIN immediately
                                    if (conn_to_state_map->state.SendBuffer.GetSize() == 0) {    // no remaining data to send
                                        sendFIN_ACK(mux, *conn_to_state_map);

                                        conn_to_state_map->state.stateOfcnx = FIN_WAIT1;
                                    } else {    // more remaining data to send
                                        conn_to_state_map->state.stateOfcnx = SEND_DATA; // will send remainder of queued data before closing
                                    }
                                    break;
                                case SEND_DATA:
                                case FIN_WAIT1:
                                case FIN_WAIT2:
                                    break;  // already received a CLOSE from user
                                case CLOSE_WAIT:
                                    // send a FIN and advance to LAST_ACK state
                                    {
                                        conn_to_state_map->state.stateOfcnx = LAST_ACK;

                                        sendFIN_ACK(mux, *conn_to_state_map);

                                        // partner has already sent FIN, immediately tell user connection is closed
                                        SockRequestResponse reply = SockRequestResponse(STATUS, req.connection, Buffer(), 0, EOK);
                                        SendSocketMessageWrapper(sock, reply);

                                        break;
                                    }
                                case CLOSING:
                                case LAST_ACK:
                                case TIME_WAIT:
                                    break;  // remain in the same state
                            }
                        } else {    // the connection is already closed
                            SockRequestResponse reply = SockRequestResponse(STATUS, req.connection, Buffer(), 0, ENOMATCH);
                            SendSocketMessageWrapper(sock, reply);
                        }
                        
                        break;
                    }
                case STATUS:                    
                {
                        // remove the related outstanding request, remove the read data from Receive Buffer, send another TCP WRITE if Receive Buffer is not empty
                        for (deque<SockRequestResponse>::iterator sock_req = sock_req_list.begin(); sock_req != sock_req_list.end(); ++sock_req) {
                            ConnectionList<MyTCPState>::iterator conn_to_state_map = clist.FindMatching(req.connection);
                            if (sock_req->type == WRITE && sock_req->connection.Matches(conn_to_state_map->connection)) {  // the user read bytes from this connection
                                if (conn_to_state_map != clist.end()) {   // found a matching connection
                                    conn_to_state_map->state.RecvBuffer.ExtractFront(req.bytes);    // remove the read data from the buffer
                                    sock_req_list.erase(sock_req);  // remove the related outstanding request

                                    // send another TCP WRITE if RecvBuffer is not empty
                                    if (conn_to_state_map->state.RecvBuffer.GetSize() > 0) {
                                        SendTCPWrite(sock, *conn_to_state_map, sock_req_list);
                                    }
                                }

                                break;
                            } else if (sock_req->type == CLOSE && sock_req->connection.Matches(conn_to_state_map->connection)) {    // the user responded to TCP CLOSE
                                if (conn_to_state_map != clist.end()) { // found a matching connection
                                    sock_req_list.erase(sock_req);  // remove the request to indicate the user acknowledged the CLOSE request
                                }

                                break;
                            } else {    // the connection did not match
                                break;
                            }
                        }
                        
                        break;
                    }
                default:
                    break;
            }
	    }  // end sock handling
	}

	if (event.eventtype == MinetEvent::Timeout) {
        ConnectionList<MyTCPState>::iterator conn_to_state_map = clist.FindEarliest();
        if (conn_to_state_map != clist.end()) {   // found the connection whose timeout expired
            switch (conn_to_state_map->state.stateOfcnx)
            {
                case SYN_SENT:
                    // resend SYN -- the conn_to_state_map should be set to the correct state
                    {
                        conn_to_state_map->state.tmrTries++; // track retransmission attempts
                        if (conn_to_state_map->state.tmrTries >= NUM_SYN_TRIES) {  // give up on trying to establish a connection
                            sendRST(mux, *conn_to_state_map); 
                            clist.erase(conn_to_state_map); // close the connection
                            zeroByteTCPWrite(sock, sock_req_list, conn_to_state_map->connection, ECONN_FAILED); // let the user know that the connection failed to be established
                        }

                        conn_to_state_map->state.SetLastSent(conn_to_state_map->state.GetLastSent() - 1); // reset last sent in order for resent packet to have same sequence number

                        sendSYN(mux, *conn_to_state_map, false);   // don't send data

                        break;
                    }
                case SYN_RCVD:
                    break;  // shouldn't encounter a timeout here since we immediately proceed to SYN_SENT1
                case SYN_SENT1:
                    // resend SYN-ACK
                    {
                        conn_to_state_map->state.tmrTries++; // track retransmission attempts
                        if (conn_to_state_map->state.tmrTries >= NUM_SYN_TRIES) {  // give up on trying to establish a connection
                            sendRST(mux, *conn_to_state_map);
                            clist.erase(conn_to_state_map); // close the connection
                            zeroByteTCPWrite(sock, sock_req_list, conn_to_state_map->connection, ECONN_FAILED); // let the user know that the connection failed to be established
                        }

                        conn_to_state_map->state.SetLastSent(conn_to_state_map->state.GetLastSent() - 1); // reset last sent in order for resent packet to have same sequence number

                        sendSYN_ACK(mux, *conn_to_state_map, false);   // don't send data

                        break;
                    }
                case ESTABLISHED:   // when FIN is sent, immediately advanced to FIN_WAIT1
                case SEND_DATA: // when FIN is sent, immediately advanced to FIN_WAIT1
                case CLOSE_WAIT:    // when FIN is sent, immediately advanced to LAST_ACK
                    // resend data
                    {
                        conn_to_state_map->state.tmrTries++; // track retransmission attempts
                        conn_to_state_map->state.SetLastSent(conn_to_state_map->state.GetLastAcked());  // reset last_sent

                        if (conn_to_state_map->state.rwnd == 0) {   // the partner has no room to receive data (or hasn't told the host that it has room yet)    
                            // send up to a full packet's worth of data irrespective of partner window size
                            conn_to_state_map->state.SetSendRwnd(conn_to_state_map->state.N); // lets BuildPacket attach data up to send window size
                            sendACK(mux, *conn_to_state_map, true); // send data
                            conn_to_state_map->state.SetSendRwnd(0);  // restore accurate partner receive window length
                        } else {    // the partner has room to receive data
                            // note that we might end sending more data than was transmitted in the dropped packet -- this is good for performance
                            sendACK(mux, *conn_to_state_map, true); // send data
                        }

                        break;
                    }
                case FIN_WAIT1:
                    // resend FIN
                    {
                        conn_to_state_map->state.tmrTries++; // track retransmission attempts
                        conn_to_state_map->state.SetLastSent(conn_to_state_map->state.GetLastSent() - 1); // reset last sent in order for resent packet to have same sequence number

                        sendFIN_ACK(mux, *conn_to_state_map);   // don't send data

                        break;
                    }
                case FIN_WAIT2: // the last sent packet (a FIN) was already ACKed
                    break;  // shouldn't encounter a timeout here
                case LAST_ACK:
                    // resend FIN
                    {
                        conn_to_state_map->state.tmrTries++; // track retransmission attempts
                        conn_to_state_map->state.SetLastSent(conn_to_state_map->state.GetLastSent() - 1); // reset last sent in order for resent packet to have same sequence number

                        sendFIN_ACK(mux, *conn_to_state_map);   // don't send data

                        break;
                    }
                case CLOSING:
                    break; // shouldn't encounter a timeout here
                case TIME_WAIT:
                    clist.erase(conn_to_state_map); // close the connection
                    break;
            }
        }
	}

    UpdateEventLoopTimeout(clist, event_loop_timeout, time_before_block);

    }   // end event loop

    MinetDeinit();

    return 0;
}


// Send Packet p out to Minet socket with fd handle
int SendPacketWrapper(const MinetHandle &handle, const Packet &p)
{   
    int status = MinetSend(handle, p);
    return status;
}

// Calculate initial sequence number (see RFC p. 33 of the pdf)
// Reads a value off of a (fictitious) 32-bit clock which increments every four microseconds (and has been running since the epoch)
void InitializeSequenceNumber(unsigned int &sequence_num)
{                   
    // WILL ASSUME size_t suseconds_t, AND unsigned long long HAVE THE SAME SIZE AS THEY HAVE ON THE LAB MACHINES (tested on lab machine 14)
    //      sizeof(size_t) == 4, sizeof(suseconds_t) == 4, sizeof(unsigned long long) == 8
    Time current_time = Time();

    unsigned long long microseconds_since_epoch = current_time.tv_sec * 1000000LL + current_time.tv_usec;   // cast one million to a long long so that we do 64-bit multiplication instead of 32-bit multiplication
    unsigned long long microseconds_since_clock_expired = microseconds_since_epoch & 0x3FFFFFFFFLL; // 0x3FFFFFFFF == 2^34 - 1, the appropriate bitmask to get the lower 34 order bits

    // note that microseconds_since_clock_expired is bounded above by 2^34, so dividing by four gives us a 32-bit sequence number
    sequence_num = (unsigned int) (microseconds_since_clock_expired >> 2); // divide by 4 microseconds per clock tick
}

// Returns a Packet constructed based on state of conn_to_state_map; will attach data from SendBuffer iff send_data_if_possible
// Refer to p. 21 of the RFC pdf for details on the TCP header structure
Packet BuildPacket(ConnectionToStateMapping<MyTCPState> &conn_to_state_map, unsigned char &flags, bool send_data_if_possible)
{
    Packet p;
    Connection connection = conn_to_state_map.connection;
    MyTCPState state = conn_to_state_map.state;

    Buffer data_to_send = Buffer();
    if (send_data_if_possible) {
        data_to_send = GetPayloadToSend(conn_to_state_map.state);
        if (data_to_send.GetSize() > 0) { // can send data
            p = Packet(data_to_send);
        }
    }

    // configure the IP header
    IPHeader iph = IPHeader();
    iph.SetSourceIP(connection.src);
    iph.SetDestIP(connection.dest);
    iph.SetProtocol(connection.protocol);
    iph.SetTotalLength(IP_HEADER_BASE_LENGTH + TCP_HEADER_BASE_LENGTH + data_to_send.GetSize());    // account for the data being sent
    iph.SetTOS(conn_to_state_map.state.precedence << 5);    // set the leftmost 3 bits of TOS to reflect the precedence of the packet
    p.PushFrontHeader(iph);

    // configure the TCP header
    TCPHeader tcph = TCPHeader();
    tcph.SetSourcePort(connection.srcport, p);
    tcph.SetDestPort(connection.destport, p);

    // connection state
    tcph.SetSeqNum(state.GetLastSent() + 1, p);
    if (IS_ACK(flags)) {
        tcph.SetAckNum(state.last_recvd + 1, p);    // ACK number is the next partner sequence number expected
    } else {
        tcph.SetAckNum(0, p);   // set ACK field to zero for non-ACKs
    }

    tcph.SetWinSize(state.GetRwnd(), p); // note that this communicates the available room in THIS host's receive buffer, not the partner's
    tcph.SetFlags(flags, p);

    // TCP header length will be constant since options are unsupported
    tcph.SetHeaderLen(TCP_HEADER_BASE_LENGTH / 4, p);   // TCP_HEADER_BASE_LENGTH is in number of 32-bit words

    // push the TCP header
    p.PushBackHeader(tcph);

    // update last_sent when data is sent
    if (data_to_send.GetSize() > 0) {
        state.SetLastSent(state.GetLastSent() + data_to_send.GetSize());
    }

    // update last sent for SYN/FIN packets
    if (IS_SYN(flags) || IS_FIN(flags)) {
        state.SetLastSent(state.GetLastSent() + 1); // SYN/FIN implicitly counts as one byte
    }

    // set retransmission timer on if need be
    // note an ACK with no data is inconclusive (if it is a duplicate ACK , timer should be on, but if not timer should be turned off)
    if (IS_SYN(flags) || IS_FIN(flags) || (IS_ACK(flags) && data_to_send.GetSize() > 0)) {  // sending a packet that we expect to be ACKed
        conn_to_state_map.timeout = MIN_MACRO(TRANSMISSION_TIMER_UPPER_BOUND, MAX_MACRO(TRANSMISSION_TIMER_LOWER_BOUND, BETA * conn_to_state_map.state.srtt));  // see p. 41 of the RFC
        conn_to_state_map.bTmrActive = true;    // note that the timeout is already configured to the appropriate duration when we process ACKs of data
        state.packet_last_transmitted = Time();   // mark when this packet was sent
    }
    
    // copy changes to object in connection list
    conn_to_state_map.state = state;

    return p;
}

// Returns the size of the user data included in the packet with IP header iph and TCP header tcph (in bytes)
size_t GetPayloadSize(IPHeader &iph, TCPHeader &tcph)
{
    unsigned short total_len;
    iph.GetTotalLength(total_len);
    unsigned char iph_len;
    iph.GetHeaderLength(iph_len);   // number of 4-byte words
    iph_len *= 4;   // number of bytes
    unsigned char tcph_len;
    tcph.GetHeaderLen(tcph_len);    // number of 4-byte words
    tcph_len *= 4;  // number of bytes
    size_t payload_size = total_len - iph_len - tcph_len;    // size of the user data in payload in bytes

    return payload_size;
}

// Adds data from payload to RecvBuffer; sends data to user through sock if no outstanding TCP WRITEs
// Returns true if and only if the payload contains user data and it was successfully received into Receive Buffer
bool ReceivePayload(Buffer &payload, IPHeader &iph, TCPHeader &tcph, ConnectionToStateMapping<MyTCPState> &conn_to_state_map
    , deque<SockRequestResponse> &sock_req_list, MinetHandle sock)
{
   size_t payload_size = GetPayloadSize(iph, tcph);
    if (payload_size > 0) { // there is data to receive
        unsigned int partner_sequence_num;
        tcph.GetSeqNum(partner_sequence_num);

        if (conn_to_state_map.state.SetLastRecvd(partner_sequence_num, payload_size)) {    // can successfully receive, this call also updates last_recvd
            conn_to_state_map.state.RecvBuffer.AddBack(payload);    // copy to RecvBuffer -- note rwnd automatically adjusts to reflect new size of RecvBuffer

            // if there are no pending TCP writes, should send a TCP write of the entire receive buffer to socket
            bool is_existing_write_req = false;
            for (deque<SockRequestResponse>::iterator sock_req = sock_req_list.begin(); sock_req != sock_req_list.end(); ++sock_req) {
                if (sock_req->type == WRITE && sock_req->connection.Matches(conn_to_state_map.connection)) {  // there is already a WRITE sent for this connection
                    is_existing_write_req = true;
                    break;
                }
            }

            if (!is_existing_write_req) {   // there are no existing write requests to the socket
                SendTCPWrite(sock, conn_to_state_map, sock_req_list);
            }

            return true;
        } else {    // cannot successfully receive, likely there is not enough room in RecvBuffer
            return false;      
        }
    } else {    // there is no data to receive
        return false;
    }

}

// Chooses the nearest timeout to expire to be the next event_loop_timeout; marks the time before event loop blocks again
void UpdateEventLoopTimeout(ConnectionList<MyTCPState> &clist, double &event_loop_timeout, Time &time_before_block)
{
    // iterate through the connections to pick lowest timeout as the timeout for event loop
    ConnectionList<MyTCPState>::iterator conn_to_state_map = clist.FindEarliest();
    if (conn_to_state_map != clist.end()) {
        event_loop_timeout = conn_to_state_map->timeout;
    }

    // mark the time before blocking again
    time_before_block = Time();
}

// Extracts IP header from p into iph, TCP header from p into tcph
void ExtractHeadersFromPacket(Packet &p, IPHeader &iph, TCPHeader &tcph)
{
    // extract the TCP header
    unsigned tcph_len = TCPHeader::EstimateTCPHeaderLength(p);
    p.ExtractHeaderFromPayload<TCPHeader>(tcph_len);

    iph = p.FindHeader(Headers::IPHeader);  // the IP header is already included in the list of packet headers
    tcph = p.FindHeader(Headers::TCPHeader);
}

// Returns true if and only if a duplicate ACK was received or some sent data has been ACked
bool IsExpectedAckNum(unsigned int &partner_ack_num, MyTCPState &state)
{
    // consequence of stop-and-wait protocol, the only valid ACK is of the last sent packet (the only packet in flight)
    // see p. 69 of the RFC
    return  IsDuplicateAck(partner_ack_num, state) || IsSentDataAcked(partner_ack_num, state);
}

// Returns true if and only if some sent data (or SYN or FIN) has been ACKed
bool IsSentDataAcked(unsigned int &partner_ack_num, MyTCPState &state)
{
    if (state.GetLastAcked() + 2 <= state.GetLastSent() + 1) {  // no overflow
        return partner_ack_num >= state.GetLastAcked() + 2 && partner_ack_num <= state.GetLastSent() + 1;
    } else {    // overflow
        return partner_ack_num >= state.GetLastAcked() + 2 || partner_ack_num <= state.GetLastSent() + 1;
    }
    
}

// Returns true if and only if no data (or SYN or FIN) has been ACKed
bool IsDuplicateAck(unsigned int &partner_ack_num, MyTCPState &state)
{
    return partner_ack_num == state.GetLastAcked() + 1;
}

// Returns the number of bytes acked by partner_ack_num given current state
int GetBytesAcked(unsigned int &partner_ack_num, MyTCPState &state)
{
    if (!IsExpectedAckNum(partner_ack_num, state)) {    // invalid ACKs cannot ACK any bytes
        return 0;
    }

    if (partner_ack_num >= state.GetLastAcked() + 1) {  // no overflow
        return partner_ack_num - (state.GetLastAcked() + 1);
    } else {    // overflow
        return SEQ_LENGTH_MASK - state.GetLastAcked() + partner_ack_num;  // SEQ_LENGTH_MASK == 2^32 - 1, defined in tcpstate.h
    }
}

// Returns true if and only if the incoming sequene number is acceptable
bool IsAcceptableSegment(unsigned int &partner_seq_num, size_t &payload_size, MyTCPState &state)
{
    // test the incoming sequence number to see if segment is acceptable
    // an acceptable segment means that it has the next sequence number expected and the segment size is not too large
        // see p. 69 of the RFC
    if (partner_seq_num != state.GetLastRecvd() + 1) {  // sequence number is invalid for stop-and-wait protocol
        return false;
    }

    if (payload_size == 0) {    // the packet does not contain any user data
        return true;
    } else {    // the packet contains some user data
        if (payload_size <= TCP_MAXIMUM_SEGMENT_SIZE) {
            return true;
        } else {    // incoming packet data is larger than maximum segment size
            return false;
        }
    }
}

// Updates connection state based on received ACK
// b_syn_or_fin determines whether to count extra byte in ack number from a previously sent SYN or FIN via parameter
bool ProcessAck(unsigned int &partner_ack_num, unsigned char &flags, ConnectionToStateMapping<MyTCPState> &conn_to_state_map, Time &time_at_event, bool b_syn_or_fin)
{
    if (!IS_ACK(flags)) {   // the packet received was not an ACK
        return false;
    }

    if (!IsExpectedAckNum(partner_ack_num, conn_to_state_map.state)) {  // partner ACK number not in range of sent data
        return false;
    }

    if (IsSentDataAcked(partner_ack_num, conn_to_state_map.state)) {    // need to update srtt for future timeouts
        Time rtt = time_at_event - conn_to_state_map.state.packet_last_transmitted; // calculate the RTT 
        conn_to_state_map.state.srtt = ALPHA * conn_to_state_map.state.srtt + (1 - ALPHA) * (double) rtt;
    }

    // clear the send buffer of any ACKed data
    unsigned int bytes_acked = partner_ack_num;
    if (b_syn_or_fin) {
        bytes_acked--;  // don't clear data for the sequence number bits taken up by SYNs and FINs
    }
    conn_to_state_map.state.SetLastAcked(bytes_acked);  // clears the Send Buffer

    conn_to_state_map.state.tmrTries = 0;   // reset retransmission counter

    // configure timer
    if (conn_to_state_map.state.rwnd == 0) {    // the partner has no room to receive data
        // turn on persist timer to keep probing partner for when their window reopens
        conn_to_state_map.timeout = MSL_TIME_SECS;  // duration of persist timer, see p. 352 of Illustrated
        conn_to_state_map.bTmrActive = true;
    } else {    // the partner has room to receive data
        conn_to_state_map.bTmrActive = false;   // turn off the timer
    }

    if (b_syn_or_fin) {
        // count the acknowledged SYN or FIN bit, without modifying the send buffer
        conn_to_state_map.state.last_acked++;
    }

    return true;
}

// Configures connection with the appropriate IP addresses from iph and TCP port numbrs from tcph
// Considers this machine to be the source and its partner to be the destination
void ConfigureIncomingConnection(IPHeader &iph, TCPHeader &tcph, Connection &connection)
{
    // set source and dest IP address
    iph.GetDestIP(connection.src);
    iph.GetSourceIP(connection.dest);
    
    // set source and dest port number
    tcph.GetDestPort(connection.srcport);
    tcph.GetSourcePort(connection.destport);
    
    // set the protocol
    iph.GetProtocol(connection.protocol);
}

// Advances the timeouts for all connections with an active timer after elapsed_time seconds have occurred
void AdvanceTimeouts(ConnectionList<MyTCPState> &clist, double &elapsed_time)
{
    for (ConnectionList<MyTCPState>::iterator conn_to_state_map = clist.begin(); conn_to_state_map != clist.end(); ++conn_to_state_map) {
        if (conn_to_state_map->bTmrActive) {
            double new_timeout = (double) conn_to_state_map->timeout - elapsed_time;
            if (new_timeout < 0) {  // floating point/clock imprecision
                new_timeout = 0;    // round back to zero
            }
            conn_to_state_map->timeout = new_timeout;
        }
    }
}

// Returns the user data from SendBuffer that may be sent at the moment based on current state
Buffer GetPayloadToSend(MyTCPState &state)
{
    // note that already sent data may be retransmitted -- this is intended behavior
    bool send_packet = state.SendBuffer.GetSize() > 0 // there is queued data to send
        && state.rwnd > 0; // partner can accept at least one byte of data

    if (send_packet) {  // a packet with some data can be sent
        unsigned int bytes_to_send = MIN_MACRO(state.SendBuffer.GetSize(), state.rwnd);
        bytes_to_send = MIN_MACRO(bytes_to_send, TCP_MAXIMUM_SEGMENT_SIZE);

        char * p_data_to_send = (char *) malloc(bytes_to_send);
        state.SendBuffer.GetData(p_data_to_send, bytes_to_send, 0);
        Buffer payload_to_send = Buffer(p_data_to_send, bytes_to_send);
        free(p_data_to_send);
        return payload_to_send;
    } else {    // no packet with data can be sent
        return Buffer();    // empty buffer
    }
}

// Sends a TCP WRITE with the entire contents of RecvBuffer to sock; Appends the WRITE to sock_req_list
void SendTCPWrite(MinetHandle sock, ConnectionToStateMapping<MyTCPState> &conn_to_state_map, deque<SockRequestResponse> &sock_req_list)
{
    // send a message to the socket
    SockRequestResponse message = SockRequestResponse(WRITE, conn_to_state_map.connection, conn_to_state_map.state.RecvBuffer
        , conn_to_state_map.state.RecvBuffer.GetSize(), EOK); // despite the Minet TCP/IP Stack saying on p. 16 that all other fields are ignored, in our experience applications would terminate early if they received non-zero errors/inaccurate byte counts
    SendSocketMessageWrapper(sock, message);

    sock_req_list.push_back(message);   // add the message to the list
}

// Sends a TCP CLOSE to sock with error code given by error
void SendTCPClose(MinetHandle sock, deque<SockRequestResponse> &sock_req_list, Connection &connection, int error)
{
    SockRequestResponse message = SockRequestResponse(CLOSE, connection, Buffer(), 0, error);
    SendSocketMessageWrapper(sock, message);
    sock_req_list.push_back(message);
}

// Send message to sock
void SendSocketMessageWrapper(MinetHandle sock, SockRequestResponse &message)
{
    MinetSend(sock, message);
}

// Initiates an active open on connection (sends a SYN)
void activeOpen(MinetHandle mux, MinetHandle sock, ConnectionList<MyTCPState> &clist, Connection &connection)
{
    // create a new TCP State object
    unsigned int sequence_num;
    InitializeSequenceNumber(sequence_num);

    // initial state is SYN_SENT (the initiator of the connection gets SYN_SENT)
    // timertries will be zero (have not retransmitted on timeout yet)
    MyTCPState state = MyTCPState(sequence_num - 1, SYN_SENT, 0);  // sets last_acked == last_sent == sequence_num - 1

    // SYN packets are treated as one byte in terms of sequence numbers
    state.last_sent = sequence_num;

    state.N = TCP_MAXIMUM_SEGMENT_SIZE; // stop-and-wait protocol means only one packet allowed in flight

    // create the connection to state map object
    ConnectionToStateMapping<MyTCPState> conn_to_state_map;
    conn_to_state_map.connection = connection;
    conn_to_state_map.timeout = (double) INITIAL_TCP_TRANSMISSION_TIMER;;
    conn_to_state_map.state = state;
    conn_to_state_map.bTmrActive = true;    // set the timer for this connection to be active

    clist.push_back(conn_to_state_map); // add the connection to the list

    sendSYN(mux, conn_to_state_map, false);   // don't send data

    // immediately return STATUS with same connection
    SockRequestResponse reply = SockRequestResponse(STATUS, connection, Buffer(), 0, EOK);
    SendSocketMessageWrapper(sock, reply);
}

// Initiates a passive open on connection (enters LISTEN state, does not send any packets)
void passiveOpen(MinetHandle mux, MinetHandle sock, ConnectionList<MyTCPState> &clist, Connection &connection)
{
    // set a TCPState in the LISTEN state
    MyTCPState state = MyTCPState(0, LISTEN, 0);    // call this constructor so that the fields are initialized/set to defaults/zeroed out appropriately

    ConnectionToStateMapping<MyTCPState> conn_to_state_map;
    conn_to_state_map.connection = connection;
    conn_to_state_map.state = state;
    conn_to_state_map.bTmrActive = false;

    clist.push_back(conn_to_state_map); // add the LISTEN connection to the connection list

    // return a STATUS immediately with only the error code set
    SockRequestResponse reply = SockRequestResponse(STATUS, conn_to_state_map.connection, Buffer(), 0, EOK);
    SendSocketMessageWrapper(sock, reply);
}

// Tells the user whether or not the connection has been successfully established based on error code
void zeroByteTCPWrite(MinetHandle sock, deque<SockRequestResponse> &sock_req_list, Connection &connection, int error)
{
    // tell the user that a connection has been made (p. 15 of the Minet Stack handout)
    SockRequestResponse message = SockRequestResponse(WRITE, connection, Buffer(), 0, error);
    SendSocketMessageWrapper(sock, message);

    sock_req_list.push_back(message);   // add the message to the list
}

// Sends a SYN, sending data if possible based on send_data_if_possible
void sendSYN(MinetHandle mux, ConnectionToStateMapping<MyTCPState> &conn_to_state_map, bool send_data_if_possible)
{
    unsigned char flags = 0;
    SET_SYN(flags);
    Packet syn_packet = BuildPacket(conn_to_state_map, flags, send_data_if_possible);

    SendPacketWrapper(mux, syn_packet);
}

// Sends a SYN-ACK, sending data if possible based on send_data_if_possible
void sendSYN_ACK(MinetHandle mux, ConnectionToStateMapping<MyTCPState> &conn_to_state_map, bool send_data_if_possible)
{
    unsigned char flags = 0;
    SET_SYN(flags);
    SET_ACK(flags);
    Packet syn_ack_packet = BuildPacket(conn_to_state_map, flags, send_data_if_possible);

    SendPacketWrapper(mux, syn_ack_packet);
}

// Sends an ACK, sending data if possible based on send_data_if_possible
void sendACK(MinetHandle mux, ConnectionToStateMapping<MyTCPState> &conn_to_state_map, bool send_data_if_possible)
{
    unsigned char flags = 0;
    SET_ACK(flags);
    Packet ack_packet = BuildPacket(conn_to_state_map, flags, send_data_if_possible);

    SendPacketWrapper(mux, ack_packet);
}

// Sends a FIN-ACK
void sendFIN_ACK(MinetHandle mux, ConnectionToStateMapping<MyTCPState> &conn_to_state_map)
{
    unsigned char flags = 0;
    SET_FIN(flags);
    SET_ACK(flags);
    Packet fin_ack_packet = BuildPacket(conn_to_state_map, flags, false);   // no more data to send

    SendPacketWrapper(mux, fin_ack_packet);
}

// Sends a RST
void sendRST(MinetHandle mux, ConnectionToStateMapping<MyTCPState> &conn_to_state_map)
{
    unsigned char flags = 0;
    SET_RST(flags);
    Packet rst_packet = BuildPacket(conn_to_state_map, flags, false);   // don't send data since connection needs to be reset

    SendPacketWrapper(mux, rst_packet);
}

// Sends a RST with sequence number sequence_num (irrespective of state.last_sent)
void sendRSTWithSeq(MinetHandle mux, ConnectionToStateMapping<MyTCPState> &conn_to_state_map, unsigned int &sequence_num)
{
    unsigned int old_sequence_num_less_one = conn_to_state_map.state.GetLastSent();    // remember our sequence number to reuse after sending RST
    conn_to_state_map.state.SetLastSent(sequence_num - 1);   // set sequence number of RST packet to sequence_num
    sendRST(mux, conn_to_state_map);
    conn_to_state_map.state.SetLastSent(old_sequence_num_less_one);    // revert to prior sequence number for future transmissions                          
}

// Send a RST-ACK
void sendRST_ACK(MinetHandle mux, ConnectionToStateMapping<MyTCPState> &conn_to_state_map)
{
    unsigned char flags = 0;
    SET_RST(flags);
    SET_ACK(flags);
    Packet rst_ack_packet = BuildPacket(conn_to_state_map, flags, false);   // don't send data since connection needs to be reset

    SendPacketWrapper(mux, rst_ack_packet);
}

// Restarts the 2 MSL timeout for conn_to_state_map
void restartTIME_WAITtimer(ConnectionToStateMapping<MyTCPState> &conn_to_state_map)
{
    conn_to_state_map.timeout = 2 * MSL_TIME_SECS;
    conn_to_state_map.bTmrActive = true;
}
