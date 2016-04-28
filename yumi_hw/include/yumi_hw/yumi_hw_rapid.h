#ifndef __YUMI_HW_RAPID_H 
#define __YUMI_HW_RAPID_H

#include "yumi_hw/yumi_hw.h"
#include <boost/thread/mutex.hpp>
#include <boost/thread.hpp>

#include <ros/ros.h>
#include "simple_message/message_handler.h"
#include "simple_message/message_manager.h"
#include "simple_message/messages/joint_message.h"
#include "simple_message/smpl_msg_connection.h"
#include "simple_message/socket/tcp_socket.h"
#include "simple_message/socket/tcp_client.h"

#define N_YUMI_JOINTS 14

/**
  * Overrides message handler: keeps joint states thread-safe.
  */
class YumiJointStateHandler : public industrial::message_handler::MessageHandler {
    using industrial::message_handler::MessageHandler::init;
    
    private:
	float joint_positions[N_YUMI_JOINTS];
	float joint_command[N_YUMI_JOINTS];
	bool first_iteration;
	boost::mutex data_buffer_mutex;

    public:
	bool getJointStates(float (&jnts)[N_YUMI_JOINTS]) {
	    data_buffer_mutex.lock();
	    memcpy(&jnts,&joint_positions,N_YUMI_JOINTS*sizeof(float));
	    data_buffer_mutex.unlock();
	}

	bool setJointCommands(float (&jnts)[N_YUMI_JOINTS]) {
	    data_buffer_mutex.lock();
	    memcpy(&joint_command,&jnts,N_YUMI_JOINTS*sizeof(float));
	    data_buffer_mutex.unlock();
	}

	bool init(industrial::smpl_msg_connection::SmplMsgConnection* connection)
	{
	    first_iteration = true;
	    return init((int)industrial::simple_message::StandardMsgTypes::JOINT, connection);
	}


    protected:
	bool internalCB(industrial::simple_message::SimpleMessage& in)
	{
	    industrial::joint_message::JointMessage joint_msg;
	    bool rtn = true;

	    if (!joint_msg.init(in))
	    {
		ROS_ERROR("Failed to initialize joint message");
		return false;
	    }

	    industrial::shared_types::shared_real joint_value_from_msg;

	    data_buffer_mutex.lock();
	    for(int i=0; i<N_YUMI_JOINTS; i++) {
		if (joint_msg.getJoints().getJoint(i, joint_value_from_msg))
		{
		    joint_positions[i] = joint_value_from_msg;
		}
		else
		{
		    rtn = false;
		}
	    }
	    data_buffer_mutex.unlock();

	    // Reply back to the controller if the sender requested it.
	    if (industrial::simple_message::CommTypes::SERVICE_REQUEST == joint_msg.getMessageType())
	    {
		industrial::simple_message::SimpleMessage reply;
		joint_msg.toReply(reply, rtn ? industrial::simple_message::ReplyTypes::SUCCESS : industrial::simple_message::ReplyTypes::FAILURE);
		this->getConnection()->sendMsg(reply);
	    }

	    //if first call back, then mirror state to command
	    if(first_iteration) {
		data_buffer_mutex.lock();
		memcpy(&joint_command,&joint_positions,N_YUMI_JOINTS*sizeof(float));
		data_buffer_mutex.unlock();
		first_iteration = false;	
	    }
	    //TODO: format trajectory request message

	    //TODO: send back on conncetion 
	    return rtn;
	}

};

/**
  * Keep a connection to the robot and send and receive joint states
  */
class YumiRapidInterface {
    private:
	boost::thread RapidCommThread_;
	
	///industrial connection
	industrial::tcp_client::TcpClient default_tcp_connection_; //?
	//industrial::tcp_client::RobotStatusRelayHandler default_robot_status_handler_; //?

	industrial::smpl_msg_connection::SmplMsgConnection* connection_;
	industrial::message_manager::MessageManager manager_;
	YumiJointStateHandler js_handler;

	bool stopComm_;

	virtual void RapidCommThreadCallback()
	{
	    while(!stopComm_) {
		manager_.spinOnce();
	    }
	    return;
	}

    public:
	YumiRapidInterface() { 
	    this->connection_ = NULL;
	    stopComm_ = true;
	}

	~YumiRapidInterface() { 
	    stopThreads();
	}
	
	void stopThreads() {
	    stopComm_ = true;
	    RapidCommThread_.join();
	}

	void startThreads() {
	    if(!stopComm_) {
		boost::thread(boost::bind(&YumiRapidInterface::RapidCommThreadCallback,this ));
	    }
	}

	void getCurrentJointStates(float (&joints)[N_YUMI_JOINTS]) {
	    js_handler.getJointStates(joints);	    
	}

	void setJointTargets(float (&joints)[N_YUMI_JOINTS]) {
	    js_handler.setJointCommands(joints);
	}


	bool init(std::string ip = "", int port = industrial::simple_socket::StandardSocketPorts::STATE) {
	    //initialize connection 
	    char* ip_addr = strdup(ip.c_str());  // connection.init() requires "char*", not "const char*"
	    ROS_INFO("Robot state connecting to IP address: '%s:%d'", ip_addr, port);
	    default_tcp_connection_.init(ip_addr, port);
	    free(ip_addr);

	    connection_ = &default_tcp_connection_;
	    connection_->makeConnect();

	    //initialize message manager
	    manager_.init(connection_);

	    //initialize message handler
	    js_handler.init(connection_);

	    //register handler to manager
	    manager_.add(&js_handler,false);

	    stopComm_ = false;
	}
  
};

class YumiHWRapid : public YumiHW
{

public:
  YumiHWRapid() : YumiHW() {
      isInited = false;
      isSetup = false;
  }
  
  ~YumiHWRapid() { 
      robot_interface.stopThreads();
  }

  float getSampleTime(){return sampling_rate_;};
	
  void setup(std::string ip_ = "", int port_ = industrial::simple_socket::StandardSocketPorts::STATE) {
      ip = ip_;
      port = port_;
      isSetup = true;
  }

  // Init, read, and write, with FRI hooks
  bool init()
  {
    if (isInited) return false;
    if(!isSetup) {
	ROS_ERROR("IP and port of controller are not set up!");
	return false;
    }

    robot_interface.init(ip,port);
    robot_interface.startThreads();
    isInited = true;

    return true;
  }

  ///copies the last received joint state out to the controller manager
  void read(ros::Time time, ros::Duration period)
  {
    if(!isInited) return;  

    data_buffer_mutex.lock();
    robot_interface.getCurrentJointStates(readJntPosition);

    for (int j = 0; j < n_joints_; j++)
    {
      joint_position_prev_[j] = joint_position_[j];
      joint_position_[j] = readJntPosition[j];
      //joint_effort_[j] = readJntEffort[j]; //TODO: read effort 
      joint_velocity_[j] = filters::exponentialSmoothing((joint_position_[j]-joint_position_prev_[j])/period.toSec(), joint_velocity_[j], 0.2); //exponential smoothing
    }
    data_buffer_mutex.unlock();
    return;
  }

  ///caches the most recent joint commands into the robot interface
  void write(ros::Time time, ros::Duration period)
  {
    if(!isInited) return;  
    enforceLimits(period);

    data_buffer_mutex.lock();
    switch (getControlStrategy())
    {
      case JOINT_POSITION:
        for (int j = 0; j < n_joints_; j++)
        {
          newJntPosition[j] = joint_position_command_[j];
        }
        break;
      
      case JOINT_VELOCITY:
	for (int j = 0; j < n_joints_; j++)
	{
	  newJntPosition[j] = joint_velocity_command_[j]*period.toSec();// + last_comm[j]; //FIXME: this should have the previous joint position in as well
	}
	break;
      //case JOINT_EFFORT:
      //break;
      default: 
	break;
    }

    robot_interface.setJointTargets(newJntPosition);
    data_buffer_mutex.unlock();
    return;
  }

private:

  ///
  YumiRapidInterface robot_interface; 
  ///
  float last_comm[N_YUMI_JOINTS];
  ///
  std::string hintToRemoteHost_;
  ///
  bool isInited, isSetup;
  ///
  float sampling_rate_;
  ///
  boost::mutex data_buffer_mutex;
  ///command buffers
  float newJntPosition[N_YUMI_JOINTS];
  ///data buffers
  float readJntPosition[N_YUMI_JOINTS];

  std::string ip;
  int port;
    

};

#endif