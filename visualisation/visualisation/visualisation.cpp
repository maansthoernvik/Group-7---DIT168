#include <iostream>
#include "visualisation.hpp"
#include <map>

/**
 * Implementation of the V2VService class as declared in v2v.hpp
 */
V2VService::V2VService(std::string ip, std::string groupId) {
    followerIp = "";
    leaderIp = "";
    myIp = ip;
    myGroupId = groupId;
    currentCarStatus.speed = 0;
    currentCarStatus.steeringAngle = 0;
    currentCarStatus.distanceFront = 0;
    currentCarStatus.distanceTraveled = 0;
    

//visualisation OD4Session
// This channel listens to the motor channel, the internal channel and the broadcast channel
// This channel is used for the visualisation microservice 
   visualisation = std::make_shared<cluon::OD4Session>(
        VISUALIZATION_CHANNEL,
        [](cluon::data::Envelope &&/*envelope*/) noexcept {}
    ); // end visualisation declaration



    /*
     * The broadcast field contains a reference to the broadcast channel which is an OD4Session. This is where
     * AnnouncePresence messages will be received.
     */
    broadcast = std::make_shared<cluon::OD4Session>(
            BROADCAST_CHANNEL,
            [this](cluon::data::Envelope &&envelope) noexcept {
                std::cout << "[OD4] ";
                switch (envelope.dataType()) {
                    case ANNOUNCE_PRESENCE: {
                        AnnouncePresence ap = cluon::extractMessage<AnnouncePresence>(std::move(envelope));
						visualisation->send(ap);
                        std::cout << "received 'AnnouncePresence' from '"
                                  << ap.vehicleIp() << "', GroupID '"
                                  << ap.groupId() << "'!" << std::endl;                              
                        
                        // Filter out yourself from announcement
                        if (ap.groupId() != myGroupId) {
                            mapOfIps.insert(std::make_pair(ap.groupId(), ap.vehicleIp()));   
                            mapOfIds[ap.vehicleIp()] = ap.groupId();
                        }
                        
                        break;
                    }
                    default: 
                        std::cout << "¯\\_(ツ)_/¯" << std::endl;
                        break;
                }
            });
    
            
    //internal communication
    internalBroadCast = std::make_shared<cluon::OD4Session>(
        INTERNAL_BROADCAST_CHANNEL,
        [this](cluon::data::Envelope &&envelope) noexcept {
            std::cout <<"[INTERNAL BR] ";
            
            
            switch (envelope.dataType()) {
                case INTERNAL_ANNOUNCE_PRESENCE: {
                    std::cout << "Announcing presence!" << std::endl;
                    announcePresence();
                    InternalAnnouncePresence msg = cluon::extractMessage<InternalAnnouncePresence>(std::move(envelope));
					visualisation->send(msg);
                    break;
                }
                case INTERNAL_FOLLOW_REQUEST: {
                    InternalFollowRequest msg = cluon::extractMessage<InternalFollowRequest>(std::move(envelope));
					visualisation->send(msg);
                    if (leaderIp.empty()){
                        followRequest(mapOfIps[msg.groupid()]);                            
                    }                      
                    std::cout << "received '" << msg.LongName() << " for group: " << msg.groupid() << std::endl;

                    break;
                 }
                 case INTERNAL_STOP_FOLLOW_REQUEST: {
                     InternalStopFollow msg = cluon::extractMessage<InternalStopFollow>(std::move(envelope));
					visualisation->send(msg);
                     std::cout << "received '" << msg.LongName() << " for group: " << msg.groupid() << std::endl;
                     stopFollow();
                     InternalStopFollowResponse retmsg;
                     retmsg.groupid(msg.groupid());
                     internalBroadCast->send(retmsg);

                     break;
                 }
                 case INTERNAL_GET_ALL_GROUPS_REQUEST: {
                     InternalGetAllGroupsRequest msg = cluon::extractMessage<InternalGetAllGroupsRequest>(std::move(envelope));
						visualisation->send(msg);

                break;
                }
                case INTERNAL_EMERGENCY_BRAKE: {
                    InternalEmergencyBrake msg = cluon::extractMessage<InternalEmergencyBrake>(std::move(envelope));
					visualisation->send(msg);
                    //Terminate communication
                    stopFollow();
                    
                    //Set steering and position to 0
                    opendlv::proxy::PedalPositionReading pedalMsg;
                    pedalMsg.percent(0);
                    motorBroadcast->send(pedalMsg);
                    opendlv::proxy::GroundSteeringReading steeringMsg;
                    steeringMsg.steeringAngle(0);
                    motorBroadcast->send(steeringMsg);
                    
                    std::cout << "received '" << msg.LongName() << std::endl;
                    break;
                }
                default: 
                    std::cout << "¯\\_(ツ)_/¯" << std::endl;
                    break;
            }
         }
    );
    
    motorBroadcast = std::make_shared<cluon::OD4Session>(
        MOTOR_BROADCAST_CHANNEL,
        [this](cluon::data::Envelope &&envelope) noexcept {
            
            using namespace opendlv::proxy;
            switch (envelope.dataType()) {
                case PEDAL_POSITION_READING: {
                    PedalPositionReading msg = cluon::extractMessage<PedalPositionReading>(std::move(envelope));
					visualisation->send(msg);
                    currentCarStatus.speed = msg.percent();
                    break;
                }
                case GROUND_STEERING_READING: {
                    GroundSteeringReading msg = cluon::extractMessage<GroundSteeringReading>(std::move(envelope));
					visualisation->send(msg);
                    currentCarStatus.steeringAngle = msg.steeringAngle();
                    break;
                }
                default:
                    std::cout << "Received a message that was not understood" << std::endl;
                    break;
            }
        }
    );

    /*
     * Each car declares an incoming UDPReceiver for messages directed at them specifically. This is where messages
     * such as FollowRequest, FollowResponse, StopFollow, etc. are received.
     */
    incoming = std::make_shared<cluon::UDPReceiver>(
            "0.0.0.0",
            DEFAULT_PORT,
            [this](std::string &&data, std::string &&sender, std::chrono::system_clock::time_point /*&&ts*/) noexcept {
                std::cout << "[UDP] ";
                std::pair<int16_t, std::string> msg = extract(data);

		        std::string senderIp = sender.substr(0, sender.find(":"));

                switch (msg.first) {
                    case FOLLOW_REQUEST: {
                        FollowRequest followRequest = decode<FollowRequest>(msg.second);
                        std::cout << "received '" << followRequest.LongName()
                                   << "' from '" << senderIp << "'!" << std::endl;

                         // After receiving a FollowRequest, check first if there is currently no car already following.
                         if (followerIp.empty()) {
                             followerIp = senderIp; // If no, add the requester to known follower slot and establish a
                             // sending channel.
                             toFollower = std::make_shared<cluon::UDPSender>(followerIp, DEFAULT_PORT);
                             followResponse();
                             
                             startReportingToFollower();
                         }
                         break;
                     }
                     case FOLLOW_RESPONSE: {
                         FollowResponse followResponse = decode<FollowResponse>(msg.second);
                         std::cout << "received '" << followResponse.LongName()
                                   << "' from '" << senderIp << "'!" << std::endl;

                        startReportingToLeader();
                        InternalFollowResponse msg;
                        msg.groupid(mapOfIds[senderIp]);
                        msg.status(1);
                        internalBroadCast->send(msg);
                        
                         break;
                     }
                     case STOP_FOLLOW: {
                         StopFollow stopFollow = decode<StopFollow>(msg.second);
                         std::cout << "received '" << stopFollow.LongName()
                                   << "' from '" << senderIp << "'!" << std::endl;

                         // Clear either follower or leader slot, depending on current role.
                         if (senderIp == followerIp) {
                             followerIp = "";
                             toFollower.reset();
                         }
                         else if (senderIp == leaderIp) {
                             leaderIp = "";
                             toLeader.reset();
                         }
                         break;
                     }
                     case FOLLOWER_STATUS: {
                         FollowerStatus followerStatus = decode<FollowerStatus>(msg.second);
                         std::cout << "received '" << followerStatus.LongName()
                                   << "' from '" << senderIp << "'!" << std::endl;
                        break;
                    }
                    case LEADER_STATUS: {
                        LeaderStatus leaderStatus = decode<LeaderStatus>(msg.second);
                        std::cout << "received '" << leaderStatus.LongName()
                                  << "' from '" << senderIp << "'!" << std::endl;
                        std::cout << "New speed = " << leaderStatus.speed() << std::endl;
                        std::cout << "New steering = " << leaderStatus.steeringAngle() << std::endl;

                         break;
                     }
                     default: std::cout << "¯\\_(ツ)_/¯" << std::endl;
                 }
             });
}

/**
 * This function sends an AnnouncePresence (id = 1001) message on the broadcast channel. It will contain information
 * about the sending vehicle, including: IP, port and the group identifier.
 */
void V2VService::announcePresence() {
    AnnouncePresence announcePresence;
    announcePresence.vehicleIp(myIp);
    announcePresence.groupId(myGroupId);
    broadcast->send(announcePresence);
}

/**
 * This function sends a FollowRequest (id = 1002) message to the IP address specified by the parameter vehicleIp. And
 * sets the current leaderIp field of the sending vehicle to that of the target of the request.
 *
 * @param vehicleIp - IP of the target for the FollowRequest
 */
void V2VService::followRequest(std::string vehicleIp) {
    if (!leaderIp.empty()) return;
    leaderIp = vehicleIp;
    toLeader = std::make_shared<cluon::UDPSender>(leaderIp, DEFAULT_PORT);
    FollowRequest followRequest;
    toLeader->send(encode(followRequest));
}

/**
 * This function send a FollowResponse (id = 1003) message and is sent in response to a FollowRequest (id = 1002).
 * This message will contain the NTP server IP for time synchronization between the target and the senderIp.
 */
void V2VService::followResponse() {
    if (followerIp.empty()) return;
    FollowResponse followResponse;
    toFollower->send(encode(followResponse));
}

/**
 * This function sends a StopFollow (id = 1004) request on the ip address of the parameter vehicleIp. If the IP address
 * is neither that of the follower nor the leader, this function ends without sending the request message.
 *
 * @param vehicleIp - IP of the target for the request
 */
void V2VService::stopFollow() {
    StopFollow stopFollow;
    if (leaderIp != "") {
        // Clear comm channels
    	toLeader->send(encode(stopFollow));
    	leaderIp = "";
     	toLeader.reset();
    }
    if (followerIp != "") {
        // Clear comm channels
	    toFollower->send(encode(stopFollow));
     	followerIp = "";
     	toFollower.reset();
    }
}

void *sendFollowerStatuses(void *v2v) {
    V2VService *v2vservice;
    v2vservice = (V2VService *)v2v;
    std::cout << "Update leader thread started!" << std::endl;

    using namespace std::chrono_literals;
    while (!v2vservice->leaderIp.empty()) {        
        // Send sensor data
        v2vservice->followerStatus();
        std::this_thread::sleep_for(500ms);
    }

    pthread_exit(NULL);
}

void V2VService::startReportingToLeader() {
    // Get time before reporting was started to break connection in case no updates are received for ~500ms
    lastLeaderUpdate = getTime();

    int status;
    pthread_t threadId;
    status = pthread_create(&threadId, NULL, sendFollowerStatuses, (void *)this);
    
    // pthread_create returns 1 if an error occured.
    if (status) {
        std::cout << "Error creating update leader thread" << std::endl;
    }
}

/**
 * This function sends a FollowerStatus (id = 3001) message on the leader channel.
 *
 * @param speed - current velocity
 * @param steeringAngle - current steering angle
 * @param distanceFront - distance to nearest object in front of the car sending the status message
 * @param distanceTraveled - distance traveled since last reading
 */
void V2VService::followerStatus() {
    if (leaderIp.empty()) return;
    FollowerStatus followerStatus;
    toLeader->send(encode(followerStatus));
}

void *sendLeaderStatuses(void *v2v) {
    V2VService *v2vservice;
    v2vservice = (V2VService *)v2v;
    std::cout << "Update follower thread started!" << std::endl;
    
    using namespace std::chrono_literals;
    while (!v2vservice->followerIp.empty()) {
        // Get sensor data
        CarStatus *currentCarStatus = v2vservice->getCurrentCarStatus();
        // Send sensor data
        v2vservice->leaderStatus(
            currentCarStatus->speed,
            currentCarStatus->steeringAngle,
            currentCarStatus->distanceTraveled
        );
        // Message frequency according to protocol.
        std::this_thread::sleep_for(500ms);
    }
    
    pthread_exit(NULL);
}

void V2VService::startReportingToFollower() {
    // Get time before reporting was started to break connection in case no updates are received for ~500ms
    lastFollowerUpdate = getTime();

    int status;
    pthread_t threadId;
    status = pthread_create(&threadId, NULL, sendLeaderStatuses, (void *)this);
    
    // pthread_create returns 1 if an error occured.
    if (status) {
        std::cout << "Error creating update follower thread" << std::endl;
    }
}

/**
 * This function sends a LeaderStatus (id = 2001) message on the follower channel.
 *
 * @param speed - current velocity
 * @param steeringAngle - current steering angle
 * @param distanceTraveled - distance traveled since last reading
 */
void V2VService::leaderStatus(float speed, float steeringAngle, uint8_t distanceTraveled) {
    if (followerIp.empty()) return;
    LeaderStatus leaderStatus;
    leaderStatus.timestamp(getTime());
    leaderStatus.speed(speed);
    leaderStatus.steeringAngle(steeringAngle);
    leaderStatus.distanceTraveled(distanceTraveled);
    toFollower->send(encode(leaderStatus));
}



CarStatus *V2VService::getCurrentCarStatus() {
    return &currentCarStatus;
}

CarStatus *V2VService::setCurrentCarStatus(struct CarStatus *newCarStatus) {
    currentCarStatus.speed = newCarStatus->speed;
    currentCarStatus.steeringAngle = newCarStatus->steeringAngle;
    currentCarStatus.distanceTraveled = newCarStatus->distanceTraveled;
    currentCarStatus.distanceFront = newCarStatus->distanceFront;
    return &currentCarStatus;
}


/**
 * Gets the current time.
 *
 * @return current time in milliseconds
 */
uint32_t V2VService::getTime() {
    timeval now;
    gettimeofday(&now, nullptr);
    return (uint32_t ) now.tv_usec / 1000;
}

/**
 * The extraction function is used to extract the message ID and message data into a pair.
 *
 * @param data - message data to extract header and data from
 * @return pair consisting of the message ID (extracted from the header) and the message data
 */
std::pair<int16_t, std::string> V2VService::extract(std::string data) {
    if (data.length() < 10) return std::pair<int16_t, std::string>(-1, "");
    unsigned int id, len;
    std::stringstream ssId(data.substr(0, 4));
    std::stringstream ssLen(data.substr(4, 10));
    ssId >> std::hex >> id;
    ssLen >> std::hex >> len;
    return std::pair<int16_t, std::string> (
            data.length() -10 == len ? id : -1,
            data.substr(10, data.length() -10)
    );
};

/**
 * Generic encode function used to encode a message before it is sent.
 *
 * @tparam T - generic message type
 * @param msg - message to encode
 * @return encoded message
 */
template <class T>
std::string V2VService::encode(T msg) {
    cluon::ToProtoVisitor v;
    msg.accept(v);
    std::stringstream buff;
    buff << std::hex << std::setfill('0')
         << std::setw(4) << msg.ID()
         << std::setw(6) << v.encodedData().length()
         << v.encodedData();
    return buff.str();
}

/**
 * Generic decode function used to decode an incoming message.
 *
 * @tparam T - generic message type
 * @param data - encoded message data
 * @return decoded message
 */
template <class T>
T V2VService::decode(std::string data) {
    std::stringstream buff(data);
    cluon::FromProtoVisitor v;
    v.decodeFrom(buff);
    T tmp = T();
    tmp.accept(v);
    return tmp;
}