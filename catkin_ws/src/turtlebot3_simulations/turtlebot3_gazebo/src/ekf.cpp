#include <ros/ros.h>
#include <geometry_msgs/Twist.h>
// #include "/opt/ros/kinetic/include/eigen_stl_containers/eigen_stl_containers.h"
#include <aruco_msgs/MarkerArray.h> // Located in devel/include/aruco_msgs/MarkerArray.h, not sure how it gets generated automatically or if it gets shifted or copied automatically
// #include "aruco.h" // Fix CMakeFiles.txt so that the aruco_ros and aruco_msgs package and msgs get discovered properly like nav_msgs etc
#include "Eigen/Dense" // Added include library EIGEN_DIRS=/usr/include/eigen3
#include <math.h>
#include <limits>
#include <nav_msgs/Odometry.h> // Found it using "rostopic info /odom". Is located in /opt/ros/kinetic/include/nav_msgs
#include <sensor_msgs/LaserScan.h> // Found it using "rostopic info /scan". Is located in /opt/ros/kinetic/include/sensor_msgs
#include <std_msgs/Header.h>
#include <std_msgs/Float64MultiArray.h>
#include <vector>

#define PI 3.14159265


double normalizeAngle(double angle)
{
	double output;
	//double rem = std::abs(angle) % PI;
	double rem = std::fmod(std::abs(angle), PI);
	double quo = (std::abs(angle) - rem) / PI;

	int oddQuo = std::fmod(quo, 2);
	// Positive angle input or 0
	if (angle >= 0)
	{
		if (oddQuo == 0)
		{
			output = rem;
		}
		else
		{
			output = -(PI - rem);
		}
	}
	// Negative Angle received
	else
	{
		if (oddQuo == 0)
		{
			output = -rem;
		}
		else
		{
			output = (PI - rem);
		}
	}

	return output;
}



class TurtleEkf
{

private:
    ros::NodeHandle n;
    ros::Publisher turtle_vel;
    ros::Publisher turtle_states;
    ros::Publisher turtle_variances;    
    ros::Subscriber turtle_odom;
    ros::Subscriber turtle_lidar;
    ros::Subscriber turtle_aruco;
    ros::Subscriber turtle_motion;
    double globalTStart;
    double prevT;
    double timeThresh;
    double angVelThresh;

    double angVel;
    double linVel;

    float INF; // float type since lidar vals are in float

    int numModelStates;
    int numLandmarks;
    int numTotStates;
    int numComponents;

    Eigen::VectorXd predictedStates; // Prev State vector also being maintained since while calc of variance, prevState vec would be reqd, 
                                // but the states would have already been updated as per motion eqns
    Eigen::VectorXd states; // ie. Corrected States
    Eigen::MatrixXd predictedVariances;    
    Eigen::MatrixXd variances;
    Eigen::MatrixXd Fx;
    Eigen::VectorXd bSeenLandmark;

    Eigen::MatrixXd RmotionCovar;
    Eigen::MatrixXd QsensorCovar;

    bool bTestMotionModelOnly;
    bool bAllDebugPrint;
    bool bSensorModelUpdating;

public:
    TurtleEkf() :
    // INF(std::numeric_limits<float>::max()), // Using such a large number can make the inversion in the update step very sensitive to numerical errors
    INF(100),
    numModelStates(3),
    numLandmarks(5),
    numTotStates(numModelStates + 2*numLandmarks),
    numComponents(2),
    predictedStates(Eigen::VectorXd::Zero(numTotStates)),
    states(Eigen::VectorXd::Zero(numTotStates)),
    predictedVariances(Eigen::MatrixXd::Zero(numTotStates, numTotStates)),    
    variances(Eigen::MatrixXd::Zero(numTotStates, numTotStates)),
    Fx (Eigen::MatrixXd::Zero(numModelStates, numTotStates)), 
    bSeenLandmark(Eigen::VectorXd::Zero(numLandmarks)),
    bTestMotionModelOnly(0),
    timeThresh(6), 
    angVelThresh(0.001),
    bAllDebugPrint(1),
    bSensorModelUpdating(0)
    {
        ROS_INFO("Started Node: efk_singleBlock");
        ROS_INFO_STREAM("Started Node: efk_singleBlock");

        // Init the publishers and subscribers
        turtle_states = n.advertise<std_msgs::Float64MultiArray>("/turtle/states", 10);
        turtle_variances = n.advertise<std_msgs::Float64MultiArray>("/turtle/variances", 10);
        turtle_motion = n.subscribe("/cmd_vel", 10, &TurtleEkf::cbMotionModel, this);  // turtle_odom = n.subscribe("/odom", 10, cbOdom);
        // turtle_odom = n.subscribe("/odom", 10, &TurtleEkf::cbOdom, this);  // turtle_odom = n.subscribe("/odom", 10, cbOdom);
        if(! bTestMotionModelOnly)
        {
            // turtle_lidar = n.subscribe("/scan", 10, &TurtleEkf::cbLidar, this);  // turtle_lidar = n.subscribe("/scan", 10, cbLidar);
            turtle_aruco = n.subscribe("/aruco_marker_publisher/markers", 10, &TurtleEkf::cbSensorModel, this);  // turtle_odom = n.subscribe("/odom", 10, cbOdom);
        }

        // Init theta to PI/2 as per X axis definition: perp to the right
        predictedStates(2) = PI/2.0;        
        states(2) = PI/2.0;

        // Set landmark variances to inf
        predictedVariances.bottomRightCorner(2*numLandmarks, 2*numLandmarks) = Eigen::MatrixXd::Constant(2*numLandmarks, 2*numLandmarks, INF);
        predictedVariances.topRightCorner(numModelStates, 2*numLandmarks) = Eigen::MatrixXd::Constant(numModelStates, 2*numLandmarks, INF);
        predictedVariances.bottomLeftCorner(2*numLandmarks, numModelStates) = Eigen::MatrixXd::Constant(2*numLandmarks, numModelStates, INF);
        variances.bottomRightCorner(2*numLandmarks, 2*numLandmarks) = Eigen::MatrixXd::Constant(2*numLandmarks, 2*numLandmarks, INF);
        variances.topRightCorner(numModelStates, 2*numLandmarks) = Eigen::MatrixXd::Constant(numModelStates, 2*numLandmarks, INF);
        variances.bottomLeftCorner(2*numLandmarks, numModelStates) = Eigen::MatrixXd::Constant(2*numLandmarks, numModelStates, INF);

        Fx.topLeftCorner(numModelStates, numModelStates) = Eigen::MatrixXd::Identity(numModelStates, numModelStates);

        Eigen::VectorXd tmp1 (3);
        tmp1 << 0.05, 0.05, 0.05; // 0.05m 0.05m 0.05rad of variance
        // tmp1 << 0.05, 0.05, 0.005;
        RmotionCovar = tmp1.asDiagonal();

        Eigen::VectorXd tmp2 (2);
        tmp2 << 0.005, 0.005; // 0.005m 0.005m of variance. Lidar data is much more reliable from simulation that estimated motion model
        // tmp2 << 0.005, 0.005;
        QsensorCovar = tmp2.asDiagonal();

        globalTStart = ros::Time::now().toSec();
        prevT = globalTStart;

    };

    ~TurtleEkf() {};

    void displayAll()
    {
        std::cout << "INIT STATES" << std::endl;
        std::cout << "INF " << INF << std::endl;
        std::cout << "numModelStates " << numModelStates << std::endl;
        std::cout << "numLandmarks " << numLandmarks << std::endl;
        std::cout << "numTotStates " << numTotStates << std::endl;
        std::cout << "numComponents " << numComponents << std::endl;
        std::cout << "predictedStates " << predictedStates << std::endl;
        std::cout << "states " << states << std::endl;
        std::cout << "predictedVariances " << predictedVariances << std::endl;
        std::cout << "variances " << variances << std::endl;
        std::cout << "Fx " << Fx << std::endl;
        std::cout << "RmotionCovar " << RmotionCovar << std::endl;
        std::cout << "QsensorCovar" << QsensorCovar << std::endl;

    }

    void cbMotionModel(const geometry_msgs::Twist &msg)
    {

        //delta_t calc
        double currT = ros::Time::now().toSec();
        double deltaT = currT - prevT;
        prevT = currT;

        linVel = msg.linear.x;
        angVel = msg.angular.z;

        std::cout << "%%%%%%%%%%%%%" << std::endl;
        std::cout << linVel << "," << angVel << std::endl;
        std::cout << deltaT << "," << angVel*deltaT << std::endl;
    
        if (std::abs(angVel) > angVelThresh)
        {
            double r = linVel/angVel;

            //?? ADD other condition of angles 
            predictedStates(0) = states(0) + ( -r*sin(states(2)) + r*sin(states(2) + angVel*deltaT) );
            predictedStates(1) = states(1) + ( +r*cos(states(2)) - r*cos(states(2) + angVel*deltaT) );
            predictedStates(2) = states(2) + angVel*deltaT;

        }
        else
        {
            double dist = linVel*deltaT;
            // ADD other condition of angles 
            predictedStates(0) = states(0) - dist*cos(states(2));
            predictedStates(1) = states(1) + dist*sin(states(2));
            predictedStates(2) = states(2);

            // double r = linVel*deltaT;
            // predictedStates(0) = states(0) + ( -r*sin(states(2)) + r*sin(states(2) + angVel*deltaT) );
            // predictedStates(1) = states(1) + ( +r*cos(states(2)) - r*cos(states(2) + angVel*deltaT) );
            // predictedStates(2) = states(2) + angVel*deltaT;

        }

        predictedStates(2) = normalizeAngle(predictedStates(2));

        if(bAllDebugPrint)
        {
            //??prt
            std::cout << "Predicted states = " << std::endl;
            std::cout << predictedStates << std::endl;
            // std::cout << "X', Y', Th': " << states(0) << ", " << states(1) << ", " << states(2) << std::endl;
        }

        // states = predictedStates;
    // };

    // void motionModelVariance(double angVel, double linVel, double deltaT)
    // {
        double derivXTh, derivYTh;
    
        if (std::abs(angVel) > angVelThresh)
        {
            double r = linVel/angVel;

            // Derivative of X and Y wrt Th
            derivXTh = -r*cos(states(2)) + r*cos(states(2) + angVel*deltaT);
            derivYTh = -r*sin(states(2)) + r*sin(states(2) + angVel*deltaT);
            // derivTh = 1 got from Identity addition
        }
        else
        {
            double dist = linVel*deltaT;
            // ADD other condition of angles 
            derivXTh = dist*sin(states(2));
            derivYTh = dist*cos(states(2));
            // derivTh = 1 got from Identity addition

            // double r = linVel*deltaT;
            // // Derivative of X and Y wrt Th
            // derivXTh = -r*cos(states(2)) + r*cos(states(2) + angVel*deltaT);
            // derivYTh = -r*sin(states(2)) + r*sin(states(2) + angVel*deltaT);
            // // derivTh = 1 got from Identity addition
        }

        Eigen::MatrixXd tmp = Eigen::MatrixXd::Zero(numModelStates, numModelStates);
        tmp(0,2) = derivXTh;
        tmp(1,2) = derivYTh;
        // Jacobian of non-linear motion model
        Eigen::MatrixXd Gt =  Eigen::MatrixXd::Identity(numTotStates, numTotStates) + Fx.transpose() * tmp * Fx;

        // Variance Calculation
        // predictedVariances = Gt * variances * Gt.transpose(); // MUST add process/gaussian noise so that in Correction step, 
                                                              //matrix inversion does not yield inv(0) and thus Nan after sometime
        predictedVariances = Gt * variances * Gt.transpose() + Fx.transpose() * RmotionCovar * Fx;

        if(bAllDebugPrint)
        {
            //??prt
            // std::cout << tmp << std::endl;
            std::cout << "Predicted variances = " << std::endl;
            std::cout << predictedVariances << std::endl;
        }

        // Perform motion update only if sensor model is not updating. Else midway through sensor model update,
        // some states might get changed causing errors. ie. Ignore motionUpdate if in the middle of sensorUpdate
        if(!bSensorModelUpdating)
        {
            states = predictedStates;
            variances = predictedVariances;

            //Send states to topic
            std_msgs::Float64MultiArray msg;
            // Set layout for multiarray
            msg.layout.dim.push_back(std_msgs::MultiArrayDimension());  
            msg.layout.dim[0].label = "states_length";
            msg.layout.dim[0].size = states.size();
            msg.layout.dim[0].stride = 1;
            // Push data
            msg.data.clear();
            for(int i = 0; i < states.size(); ++i)
            {
                msg.data.push_back(states[i]); 
            }
            turtle_states.publish(msg);

            //Send variances to topic
            // Set layout for multiarray (Row major as per docs)
            msg.layout.dim.push_back(std_msgs::MultiArrayDimension());  
            msg.layout.dim[0].label = "variances_num_rows";            
            msg.layout.dim[0].size = variances.rows();
            msg.layout.dim[0].stride = variances.cols();
            msg.layout.dim[1].label = "variances_num_cols";
            msg.layout.dim[1].size = variances.cols();
            msg.layout.dim[1].stride = 1;
            // Push data
            msg.data.clear();
            for(int i = 0; i < variances.rows(); ++i)
            {
                for(int j = 0; j < variances.cols(); ++j)
                {
                    // msg.data.push_back( variances.block(i,j,1,1) ); // mat.block(loc_row, loc_col, len_row, len_col) ... But this returns element in MatrixXd form
                    msg.data.push_back( variances(i,j) ); // This returns just the element, in Float646/double form
                }
            }
            turtle_variances.publish(msg);
        }

    };


    void cbSensorModel(const aruco_msgs::MarkerArray::Ptr &msg)
    {
        std::cout << "ARUCOARUCO" << std::endl;
        std::cout << msg->markers.size() << std::endl;

        bSensorModelUpdating = 1;
        predictedStates = states;
        predictedVariances = variances;


        if(bAllDebugPrint)
        {
            std::cout << "Predicted states before correction step = " << std::endl;
            std::cout << predictedStates << std::endl;
            std::cout << "Predicted variances before correction step = " << std::endl;
            std::cout << predictedVariances << std::endl;        
        }

        for(int i=0; i<msg->markers.size(); ++i)
        {

            aruco_msgs::Marker &marker_i = msg->markers.at(i);
            double x = marker_i.pose.pose.position.x;
            double z = marker_i.pose.pose.position.z;
            double avgRange = z;
            double headingMiddle = -std::atan2(x,z); // negated so that angle is positive to the LHS of robot

            int landmarkId = marker_i.id; //Was written for landmark indexes starting from 0. But Aruco markers from idx 1 are being used.
            int stateIdx = numModelStates + 2*landmarkId;
            std::cout << "Arucomarker idx, State idx" << std::endl;
            std::cout << landmarkId << ", " << stateIdx << std::endl;

            if(landmarkId > 10) // Sometimes landmarkId 1023 detected. Condition to ignore such a detection.
            {
                std::cout << "Detected bad aruco marker: " << landmarkId<< std::endl;
                continue;
            }

            if(bAllDebugPrint)
            {
                std::cout << "stateIdx = " << stateIdx << std::endl;
            }
            if ( !bSeenLandmark(landmarkId) ) // If landmark not seen before, set the prior of that landmark to global position of the landmark
            {
                // NOTE: ujx is the state in states that corsp to this j-th landmark

                // u_jx = u_tx + r*cos(phi + u_tth)
                // landmark_x = robot_x + r*cos(phi + robot_heading)
                predictedStates(stateIdx) = predictedStates(0) + avgRange * cos(headingMiddle + predictedStates(2));
                predictedStates(stateIdx+1) = predictedStates(1) + avgRange * sin(headingMiddle + predictedStates(2));

                if(bAllDebugPrint)
                {
                    std::cout << "ONE TIME STATES" << std::endl;
                    std::cout << predictedStates << std::endl;
                }

                bSeenLandmark(landmarkId) = 1;
            }

            double delx = predictedStates(stateIdx) - predictedStates(0);
            double dely = predictedStates(stateIdx+1) - predictedStates(1);
            double q = delx*delx + dely*dely;
            if(bAllDebugPrint)
            {
                std::cout << "delx, dely and q = " << std::endl;
                std::cout << delx << std::endl;
                std::cout << dely << std::endl;
                std::cout << q << std::endl;
            }

            Eigen::VectorXd zj(2);
            Eigen::VectorXd zjHat (2);
            zj << avgRange, headingMiddle;
            double tmpAngle = std::atan2(dely, delx) - predictedStates(2);
            zjHat << std::sqrt(q) , normalizeAngle(tmpAngle);
            if(bAllDebugPrint)
            {
                std::cout << "z and zHat = " << std::endl;
                std::cout << zj << std::endl;
                std::cout << zjHat << std::endl;
            }

            tmpPrint(zj, zjHat, stateIdx, tmpAngle);

            Eigen::MatrixXd Fxj = Eigen::MatrixXd::Zero( (numModelStates + numComponents), numTotStates);        
            Fxj.topLeftCorner(numModelStates,numModelStates) = Eigen::MatrixXd::Identity(numModelStates,numModelStates);
            
            Fxj(numModelStates, stateIdx) = 1;
            Fxj(numModelStates+1, stateIdx+1) = 1;
            if(bAllDebugPrint)
            {
                std::cout << "Fxj = " << std::endl;
                std::cout << Fxj << std::endl;
            }

            // Partial differential of:
            // zHat_x wrt modelX, modelY, modelTh, mx, my
            // zHat_y wrt modelX, modelY, modelTh, mx, my
            // H*z ==nt Cx or Hx in traditional state space model, but here input x for this step is estimated/expected sensor reading = zHat
            Eigen::MatrixXd H (numComponents, numModelStates + numComponents);
            Eigen::MatrixXd Hq (numComponents, numModelStates + numComponents);
            H << 
                -std::sqrt(q)*delx , -std::sqrt(q)*dely , 0    , std::sqrt(q)*delx   , std::sqrt(q)*dely ,
                dely              , -delx               , -q   , -dely               , delx;        
            Hq = (1/q) * H;
            // H = H * (1/q);
            // std::cout << "H = " << std::endl;
            // std::cout << H << std::endl;

            // (3+2)x(3+2n) = 5x5 for single landmark case, with each landmark being 2D
            Eigen::MatrixXd HFxj (numTotStates, numTotStates);
            Eigen::MatrixXd Htrans (numTotStates, numTotStates);
            HFxj = Hq * Fxj;
            if(bAllDebugPrint)
            {
                std::cout << "HFxj = " << std::endl; // Same as H since with only one obstacle, we get just F to be identity
                std::cout << HFxj << std::endl;        
            }

            Eigen::MatrixXd K (numTotStates, numComponents);
            Eigen::MatrixXd tmp (numComponents, numComponents);
            Eigen::MatrixXd tmpInv (numComponents, numComponents);
            // tmp = HFxj * predictedVariances * HFxj.transpose(); // MUST add process/gaussian noise so that in Correction step, 
                                                                //matrix inversion does not yield inv(0) and thus Nan after sometime
            Htrans = HFxj.transpose();
            tmp = HFxj * predictedVariances * Htrans + QsensorCovar;
            tmpInv = tmp.inverse(); // As explained in the doc (http://eigen.tuxfamily.org/dox-devel/cl ... 030f79f9da), mat.inverse() returns the inverse of mat, keeping mat unchanged:
            std::cout << "tmp Det" << std::endl;
            std::cout << tmp.determinant() << std::endl;            
            std::cout << "tmpInv" << std::endl;
            std::cout << tmpInv << std::endl;
            K = predictedVariances * Htrans * tmpInv; 

            if(bAllDebugPrint)
            {
                std::cout << "K = " << std::endl;
                std::cout << K << std::endl;
            }

            if( std::abs(tmp.determinant()) > 0.0001 ) // 10 power -4
            {
                predictedStates = predictedStates + K * (zj - zjHat);
                Eigen::MatrixXd Iden (numTotStates, numTotStates);
                Iden = Eigen::MatrixXd::Identity(numTotStates, numTotStates);
                predictedVariances = ( Iden - K*HFxj) * predictedVariances;
            }
            else
            {
                std::cout << "UNSTABLE" << std::endl;
            }

        } // End for each landmark

        states = predictedStates;
        variances = predictedVariances;

        //Send states to topic
        std_msgs::Float64MultiArray msgSt;
        // Set layout for multiarray
        msgSt.layout.dim.push_back(std_msgs::MultiArrayDimension());  
        msgSt.layout.dim[0].label = "states_length";
        msgSt.layout.dim[0].size = states.size();
        msgSt.layout.dim[0].stride = 1;
        // Push data
        msgSt.data.clear();
        for(int i = 0; i < states.size(); ++i)
        {
            msgSt.data.push_back(states[i]); 
        }
        turtle_states.publish(msgSt);

        //Send variances to topic
        // Set layout for multiarray (Row major as per docs)
        msgSt.layout.dim.push_back(std_msgs::MultiArrayDimension());  
        msgSt.layout.dim[0].label = "variances_num_rows";            
        msgSt.layout.dim[0].size = variances.rows();
        msgSt.layout.dim[0].stride = variances.cols();
        msgSt.layout.dim[1].label = "variances_num_cols";
        msgSt.layout.dim[1].size = variances.cols();
        msgSt.layout.dim[1].stride = 1;
        // Push data
        msgSt.data.clear();
        for(int i = 0; i < variances.rows(); ++i)
        {
            for(int j = 0; j < variances.cols(); ++j)
            {
                // msgSt.data.push_back( variances.block(i,j,1,1) ); // mat.block(loc_row, loc_col, len_row, len_col) ... But this returns element in MatrixXd form
                msgSt.data.push_back( variances(i,j) ); // This returns just the element, in Float646/double form
            }
        }
        turtle_variances.publish(msgSt);

        bSensorModelUpdating = 0;

        if(bAllDebugPrint)
        {
            std::cout << "Corrected states and variances" << std::endl;
            std::cout << states << std::endl;
            std::cout << variances << std::endl;
        }

    };

    void tmpPrint(Eigen::VectorXd z, Eigen::VectorXd zHat, int stateIdx, double tmpAngle)
    {
        std::cout << "-------------------------" << std::endl;
        std::cout << "x, mX and y, mY" << std::endl;
        std::cout << predictedStates(0) << ", " << predictedStates(1) << std::endl;
        std::cout << predictedStates(stateIdx) << ", " << predictedStates(stateIdx+1) << std::endl;
        std::cout << "diffs" << std::endl;
        std::cout << predictedStates(stateIdx) - predictedStates(0) << ", " << predictedStates(stateIdx+1) - predictedStates(1) << std::endl;
        std::cout << "theta" << std::endl;
        std::cout << predictedStates(2) << ", " << states(2) << std::endl;
        std::cout << "alpha" << std::endl;
        std::cout << tmpAngle << std::endl;
        std::cout << "z and zHat" << std::endl;
        std::cout << z(0) << ", " << z(1)*(180/PI) << std::endl;
        std::cout << zHat(0) << ", " << zHat(1)*(180/PI) << std::endl;
        std::cout << "-------------------------" << std::endl;
    }

};


int main(int argc, char** argv)
{
    // Must perform ROS init before object creation, as node handles are created in the obj constructor
    ros::init(argc, argv, "ekf");

    TurtleEkf* turtlebot = new TurtleEkf(); // Init on heap so that large lidar data isn't an issue
    turtlebot->displayAll();

    ros::spin();

    return 0;
}


//?? Other cases/components of angles to be considered in motion model?...esp when bot goaes beyond +- 180deg?
//?? Add noise to variance eqn and motion model?
//?? When sim ends and v and omega zero, what dies motion model output?? Should I kill sim little before the end and check vals
//?? For pure motion model only, check eqn wise why SIGMA ain't changing ... does makes sense since no info about landmarks so that
// remains at inf, and only motion model is present so that stays fully certain at 0. Eqn wise, tmp happens to just stay really close to 0
// Later see how it performs when some noise is incorporated. Noise to be added to v, w, th and to variance matrix as Rt.
//?? Get rid of the the static functions in the class by using the "&" template in the subscriber description line
//?? Replace all 2s everywhere with numLandmarkComponents OR numLandmarkDims
