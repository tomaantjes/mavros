/**
 * @brief IMU and attitude data parser plugin
 * @file imu.cpp
 * @author Vladimir Ermakov <vooon341@gmail.com>
 *
 * @addtogroup plugin
 * @{
 */
/*
 * Copyright 2013-2017 Vladimir Ermakov.
 *
 * This file is part of the mavros package and subject to the license terms
 * in the top-level LICENSE file of the mavros repository.
 * https://github.com/mavlink/mavros/tree/master/LICENSE.md
 */

#include <cmath>
#include <mavros/mavros_plugin.h>
#include <eigen_conversions/eigen_msg.h>

#include <sensor_msgs/Imu.h>
#include <sensor_msgs/MagneticField.h>
#include <sensor_msgs/Temperature.h>
#include <sensor_msgs/FluidPressure.h>
#include <geometry_msgs/Vector3.h>

namespace mavros {
namespace std_plugins {
/**
 * Gauss to Tesla coeff
 */
static constexpr double GAUSS_TO_TESLA = 1.0e-4;
/**
 * millTesla to Tesla coeff
 */
static constexpr double MILLIT_TO_TESLA = 1000.0;
/**
 * millRad/Sec to Rad/Sec coeff
 */
static constexpr double MILLIRS_TO_RADSEC = 1.0e-3;
/**
 * millG to m/s**2 coeff
 */
static constexpr double MILLIG_TO_MS2 = 9.80665 / 1000.0;
/**
 * millBar to Pascal coeff
 */
static constexpr double MILLIBAR_TO_PASCAL = 1.0e2;
/**
 * Radians to degrees
 */
static constexpr double RAD_TO_DEG = 180.0 / M_PI;


/**
 * @brief IMU and attitude data publication plugin
 */
class IMUPlugin : public plugin::PluginBase {
public:
	IMUPlugin() : PluginBase(),
		imu_nh("~imu"),
		has_hr_imu(false),
		has_scaled_imu(false),
		has_att_quat(false)
	{ }

	void initialize(UAS &uas_)
	{
		PluginBase::initialize(uas_);

		double linear_stdev, angular_stdev, orientation_stdev, mag_stdev;

		/**
		 * A rotation from the aircraft-frame to the base_link frame is applied.
		 * Additionally, it is reported the orientation of the vehicle to describe the
		 * transformation from the ENU frame to the base_link frame (ENU <-> base_link).
		 * THIS ORIENTATION IS NOT THE SAME AS THAT REPORTED BY THE FCU (NED <-> aircraft).
		 */
		imu_nh.param<std::string>("frame_id", frame_id, "base_link");
		imu_nh.param("linear_acceleration_stdev", linear_stdev, 0.0003);		// check default by MPU6000 spec
		imu_nh.param("angular_velocity_stdev", angular_stdev, 0.02 * (M_PI / 180.0));	// check default by MPU6000 spec
		imu_nh.param("orientation_stdev", orientation_stdev, 1.0);
		imu_nh.param("magnetic_stdev", mag_stdev, 0.0);

		setup_covariance(linear_acceleration_cov, linear_stdev);
		setup_covariance(angular_velocity_cov, angular_stdev);
		setup_covariance(orientation_cov, orientation_stdev);
		setup_covariance(magnetic_cov, mag_stdev);
		setup_covariance(unk_orientation_cov, 0.0);

		imu_pub = imu_nh.advertise<sensor_msgs::Imu>("data", 10);
		magn_pub = imu_nh.advertise<sensor_msgs::MagneticField>("mag", 10);
		temp_pub = imu_nh.advertise<sensor_msgs::Temperature>("temperature", 10);
		press_pub = imu_nh.advertise<sensor_msgs::FluidPressure>("atm_pressure", 10);
		imu_raw_pub = imu_nh.advertise<sensor_msgs::Imu>("data_raw", 10);

		/**
		 * Reset has_* flags on connection change
		 */
		enable_connection_cb();
	}

	Subscriptions get_subscriptions() {
		return {
			       make_handler(&IMUPlugin::handle_attitude),
			       make_handler(&IMUPlugin::handle_attitude_quaternion),
			       make_handler(&IMUPlugin::handle_highres_imu),
			       make_handler(&IMUPlugin::handle_raw_imu),
			       make_handler(&IMUPlugin::handle_scaled_imu),
			       make_handler(&IMUPlugin::handle_scaled_pressure),
		};
	}

private:
	ros::NodeHandle imu_nh;
	std::string frame_id;

	ros::Publisher imu_pub;
	ros::Publisher imu_raw_pub;
	ros::Publisher magn_pub;
	ros::Publisher temp_pub;
	ros::Publisher press_pub;

	bool has_hr_imu;
	bool has_scaled_imu;
	bool has_att_quat;
	Eigen::Vector3d linear_accel_vec_enu;
	Eigen::Vector3d linear_accel_vec_ned;
	ftf::Covariance3d linear_acceleration_cov;
	ftf::Covariance3d angular_velocity_cov;
	ftf::Covariance3d orientation_cov;
	ftf::Covariance3d unk_orientation_cov;
	ftf::Covariance3d magnetic_cov;

	/* -*- helpers -*- */

	/**
	 * @brief Setup 3x3 covariance matrix
	 * @param[in,out] cov	Covariance matrix
	 * @param[in,out] stdev	Standard deviation
	 * @remarks	Diagonal computed from the stdev
	 */
	void setup_covariance(ftf::Covariance3d &cov, double stdev)
	{
		std::fill(cov.begin(), cov.end(), 0.0);
		if (stdev == 0.0)
			cov[0] = -1.0;
		else {
			cov[0 + 0] = cov[3 + 1] = cov[6 + 2] = std::pow(stdev, 2);
		}
	}

	/**
	 * @brief Fill and publish IMU data message.
	 * @param[in] time_boot_ms	Message timestamp (not syncronized)
	 * @param[in,out] orientation_enu	Orientation in the base_link ENU frame
	 * @param[in,out] orientation_ned	Orientation in the aircraft NED frame
	 * @param[in,out] gyro_enu	Angular velocity/rate in the base_link ENU frame
	 * @param[in,out] gyro_ned	Angular velocity/rate in the aircraft NED frame
	 */
	void publish_imu_data(uint32_t time_boot_ms, Eigen::Quaterniond &orientation_enu,
				Eigen::Quaterniond &orientation_ned, Eigen::Vector3d &gyro_enu, Eigen::Vector3d &gyro_ned)
	{
		auto imu_ned_msg = boost::make_shared<sensor_msgs::Imu>();
		auto imu_enu_msg = boost::make_shared<sensor_msgs::Imu>();

		/** Fill message header
		 */
		imu_enu_msg->header = m_uas->synchronized_header(frame_id, time_boot_ms);
		imu_ned_msg->header = m_uas->synchronized_header("aircraft", time_boot_ms);

		tf::quaternionEigenToMsg(orientation_enu, imu_enu_msg->orientation);
		tf::quaternionEigenToMsg(orientation_ned, imu_ned_msg->orientation);
		tf::vectorEigenToMsg(gyro_enu, imu_enu_msg->angular_velocity);
		tf::vectorEigenToMsg(gyro_ned, imu_ned_msg->angular_velocity);

		/** Vector from HIGHRES_IMU or RAW_IMU
		 */
		tf::vectorEigenToMsg(linear_accel_vec_enu, imu_enu_msg->linear_acceleration);
		tf::vectorEigenToMsg(linear_accel_vec_ned, imu_ned_msg->linear_acceleration);

		imu_enu_msg->orientation_covariance = orientation_cov;
		imu_enu_msg->angular_velocity_covariance = angular_velocity_cov;
		imu_enu_msg->linear_acceleration_covariance = linear_acceleration_cov;

		imu_ned_msg->orientation_covariance = orientation_cov;
		imu_ned_msg->angular_velocity_covariance = angular_velocity_cov;
		imu_ned_msg->linear_acceleration_covariance = linear_acceleration_cov;

		/** Store attitude in base_link ENU
		 */
		m_uas->update_attitude_imu_enu(imu_enu_msg);

		/** Store attitude in aircraft NED
		 */
		m_uas->update_attitude_imu_ned(imu_ned_msg);

		/** Publish only base_link ENU message
		 */
		imu_pub.publish(imu_enu_msg);
	}

	/**
	 * @brief fill and publish IMU data_raw message; store linear acceleration for IMU data
	 * @param[in] header	Message frame_id and timestamp
	 * @param[in,out] gyro	Orientation in the base_link ENU frame
	 * @param[in,out] accel_enu	Linear acceleration in the base_link ENU frame
	 * @param[in,out] accel_ned	Linear acceleration in the aircraft NED frame
	 */
	void publish_imu_data_raw(std_msgs::Header &header, Eigen::Vector3d &gyro,
				Eigen::Vector3d &accel_enu, Eigen::Vector3d &accel_ned)
	{
		auto imu_msg = boost::make_shared<sensor_msgs::Imu>();

		/** Fill message header
		 */
		imu_msg->header = header;

		tf::vectorEigenToMsg(gyro, imu_msg->angular_velocity);
		tf::vectorEigenToMsg(accel_enu, imu_msg->linear_acceleration);

		/** Save readings
		 */
		linear_accel_vec_enu = accel_enu;
		linear_accel_vec_ned = accel_ned;

		imu_msg->orientation_covariance = unk_orientation_cov;
		imu_msg->angular_velocity_covariance = angular_velocity_cov;
		imu_msg->linear_acceleration_covariance = linear_acceleration_cov;

		/** Publish message [ENU frame]
		 */
		imu_raw_pub.publish(imu_msg);
	}

	/**
	 * @brief Publish magnetic field data
	 * @param[in] header	Message frame_id and timestamp
	 * @param[in,out] mag_field	Magnetic field in the base_link ENU frame
	 */
	void publish_mag(std_msgs::Header &header, Eigen::Vector3d &mag_field)
	{
		auto magn_msg = boost::make_shared<sensor_msgs::MagneticField>();

		/** Fill message header
		 */
		magn_msg->header = header;

		tf::vectorEigenToMsg(mag_field, magn_msg->magnetic_field);
		magn_msg->magnetic_field_covariance = magnetic_cov;

		/** Publish message [ENU frame]
		 */
		magn_pub.publish(magn_msg);
	}

	/* -*- message handlers -*- */

	/**
	 * @brief Handle ATTITUDE MAVlink message.
	 * Message specification: http://mavlink.org/messages/common/#ATTITUDE
	 * @param[in] msg	Received Mavlink msg
	 * @param[in,out] att	ATTITUDE msg
	 */
	void handle_attitude(const mavlink::mavlink_message_t *msg, mavlink::common::msg::ATTITUDE &att)
	{
		if (has_att_quat)
			return;

		/** Orientation on the NED-aicraft frame
		 */
		auto ned_aircraft_orientation = ftf::quaternion_from_rpy(att.roll, att.pitch, att.yaw);

		/** Angular velocity on the NED-aicraft frame
		 */
		auto gyro_ned = Eigen::Vector3d(att.rollspeed, att.pitchspeed, att.yawspeed);

		/** Here we have rpy describing the rotation: aircraft->NED.
		 *  We need to change this to aircraft->base_link.
		 *  And finally change it to base_link->ENU.
		 */
		auto enu_baselink_orientation = ftf::transform_orientation_aircraft_baselink(
					ftf::transform_orientation_ned_enu(ned_aircraft_orientation));

		/** Here we have the angular velocity expressed in the aircraft frame.
		 *  We need to apply the static rotation to get it into the base_link frame.
		 */
		auto gyro_enu = ftf::transform_frame_aircraft_baselink(gyro_ned);

		publish_imu_data(att.time_boot_ms, enu_baselink_orientation, ned_aircraft_orientation, gyro_enu, gyro_ned);
	}

	/**
	 * @brief Handle ATTITUDE_QUATERNION MAVlink message.
	 * Message specification: http://mavlink.org/messages/common/#ATTITUDE_QUATERNION
	 * @param[in] msg	Received Mavlink msg
	 * @param[in,out] att_q	ATTITUDE_QUATERNION msg
	 */
	void handle_attitude_quaternion(const mavlink::mavlink_message_t *msg, mavlink::common::msg::ATTITUDE_QUATERNION &att_q)
	{
		ROS_INFO_COND_NAMED(!has_att_quat, "imu", "IMU: Attitude quaternion IMU detected!");
		has_att_quat = true;

		/** Orientation on the NED-aicraft frame
		 */
		auto ned_aircraft_orientation = Eigen::Quaterniond(att_q.q1, att_q.q2, att_q.q3, att_q.q4);

		/** Angular velocity on the NED-aicraft frame
		 */
		auto gyro_ned = Eigen::Vector3d(att_q.rollspeed, att_q.pitchspeed, att_q.yawspeed);

		/** MAVLink quaternion exactly matches Eigen convention.
		 *  Here we have rpy describing the rotation: aircraft->NED.
		 *  We need to change this to aircraft->base_link.
		 *  And finally change it to base_link->ENU.
		 */
		auto enu_baselink_orientation = ftf::transform_orientation_aircraft_baselink(
					ftf::transform_orientation_ned_enu(ned_aircraft_orientation));

		/** Here we have the angular velocity expressed in the aircraft frame.
		 *  We need to apply the static rotation to get it into the base_link frame.
		 */
		auto gyro_enu = ftf::transform_frame_aircraft_baselink(gyro_ned);

		publish_imu_data(att_q.time_boot_ms, enu_baselink_orientation, ned_aircraft_orientation, gyro_enu, gyro_ned);
	}

	/**
	 * @brief Handle HIGHRES_IMU MAVlink message.
	 * Message specification: http://mavlink.org/messages/common/#HIGHRES_IMU
	 * @param[in] msg	Received Mavlink msg
	 * @param[in,out] imu_hr	HIGHRES_IMU msg
	 */
	void handle_highres_imu(const mavlink::mavlink_message_t *msg, mavlink::common::msg::HIGHRES_IMU &imu_hr)
	{
		ROS_INFO_COND_NAMED(!has_hr_imu, "imu", "IMU: High resolution IMU detected!");
		has_hr_imu = true;

		auto header = m_uas->synchronized_header(frame_id, imu_hr.time_usec);
		/** @todo Make more paranoic check of HIGHRES_IMU.fields_updated
		 */

		/** Check if accelerometer + gyroscope data are available.
		 *  Data is expressed in aircraft frame it is required to rotate to the base_link frame
		 */
		if (imu_hr.fields_updated & ((7 << 3) | (7 << 0))) {
			auto gyro = ftf::transform_frame_aircraft_baselink(Eigen::Vector3d(imu_hr.xgyro, imu_hr.ygyro, imu_hr.zgyro));

			auto accel_ned = Eigen::Vector3d(imu_hr.xacc, imu_hr.yacc, imu_hr.zacc);
			auto accel_enu = ftf::transform_frame_aircraft_baselink(accel_ned);

			publish_imu_data_raw(header, gyro, accel_enu, accel_ned);
		}

		/** Check if magnetometer data is available
		 */
		if (imu_hr.fields_updated & (7 << 6)) {
			auto mag_field = ftf::transform_frame_aircraft_baselink<Eigen::Vector3d>(
						Eigen::Vector3d(imu_hr.xmag, imu_hr.ymag, imu_hr.zmag) * GAUSS_TO_TESLA);

			publish_mag(header, mag_field);
		}

		/** Check if pressure sensor data is available
		 */
		if (imu_hr.fields_updated & (1 << 9)) {
			auto atmp_msg = boost::make_shared<sensor_msgs::FluidPressure>();

			atmp_msg->header = header;
			atmp_msg->fluid_pressure = imu_hr.abs_pressure * MILLIBAR_TO_PASCAL;

			press_pub.publish(atmp_msg);
		}

		/** Check if temperature data is available
		 */
		if (imu_hr.fields_updated & (1 << 12)) {
			auto temp_msg = boost::make_shared<sensor_msgs::Temperature>();

			temp_msg->header = header;
			temp_msg->temperature = imu_hr.temperature;

			temp_pub.publish(temp_msg);
		}
	}

	/**
	 * @brief Handle RAW_IMU MAVlink message.
	 * Message specification: http://mavlink.org/messages/common/#RAW_IMU
	 * @param[in] msg	Received Mavlink msg
	 * @param[in,out] imu_raw	RAW_IMU msg
	 */
	void handle_raw_imu(const mavlink::mavlink_message_t *msg, mavlink::common::msg::RAW_IMU &imu_raw)
	{
		if (has_hr_imu || has_scaled_imu)
			return;

		auto imu_msg = boost::make_shared<sensor_msgs::Imu>();
		auto header = m_uas->synchronized_header(frame_id, imu_raw.time_usec);

		/** @note APM send SCALED_IMU data as RAW_IMU
		 */
		auto gyro = ftf::transform_frame_aircraft_baselink<Eigen::Vector3d>(
					Eigen::Vector3d(imu_raw.xgyro, imu_raw.ygyro, imu_raw.zgyro) * MILLIRS_TO_RADSEC);
		auto accel_ned = Eigen::Vector3d(imu_raw.xacc, imu_raw.yacc, imu_raw.zacc);
		auto accel_enu = ftf::transform_frame_aircraft_baselink<Eigen::Vector3d>(accel_ned);

		if (m_uas->is_ardupilotmega()) {
			accel_ned *= MILLIG_TO_MS2;
			accel_enu *= MILLIG_TO_MS2;
		}

		publish_imu_data_raw(header, gyro, accel_enu, accel_ned);

		if (!m_uas->is_ardupilotmega()) {
			ROS_WARN_THROTTLE_NAMED(60, "imu", "IMU: linear acceleration on RAW_IMU known on APM only.");
			ROS_WARN_THROTTLE_NAMED(60, "imu", "IMU: ~imu/data_raw stores unscaled raw acceleration report.");
			linear_accel_vec_enu.setZero();
			linear_accel_vec_ned.setZero();
		}

		/** Magnetic field data
		 */
		auto mag_field = ftf::transform_frame_aircraft_baselink<Eigen::Vector3d>(
					Eigen::Vector3d(imu_raw.xmag, imu_raw.ymag, imu_raw.zmag) * MILLIT_TO_TESLA);

		publish_mag(header, mag_field);
	}

	/**
	 * @brief Handle SCALED_IMU MAVlink message.
	 * Message specification: http://mavlink.org/messages/common/#SCALED_IMU
	 * @param[in] msg	Received Mavlink msg
	 * @param[in,out] imu_raw	SCALED_IMU msg
	 */
	void handle_scaled_imu(const mavlink::mavlink_message_t *msg, mavlink::common::msg::SCALED_IMU &imu_raw)
	{
		if (has_hr_imu)
			return;

		ROS_INFO_COND_NAMED(!has_scaled_imu, "imu", "IMU: Scaled IMU message used.");
		has_scaled_imu = true;

		auto imu_msg = boost::make_shared<sensor_msgs::Imu>();
		auto header = m_uas->synchronized_header(frame_id, imu_raw.time_boot_ms);

		auto gyro = ftf::transform_frame_aircraft_baselink<Eigen::Vector3d>(
					Eigen::Vector3d(imu_raw.xgyro, imu_raw.ygyro, imu_raw.zgyro) * MILLIRS_TO_RADSEC);
		auto accel_ned = Eigen::Vector3d(Eigen::Vector3d(imu_raw.xacc, imu_raw.yacc, imu_raw.zacc) * MILLIG_TO_MS2);
		auto accel_enu = ftf::transform_frame_aircraft_baselink<Eigen::Vector3d>(accel_ned);

		publish_imu_data_raw(header, gyro, accel_enu, accel_ned);

		/** Magnetic field data
		 */
		auto mag_field = ftf::transform_frame_aircraft_baselink<Eigen::Vector3d>(
					Eigen::Vector3d(imu_raw.xmag, imu_raw.ymag, imu_raw.zmag) * MILLIT_TO_TESLA);

		publish_mag(header, mag_field);
	}

	/**
	 * @brief Handle SCALED_PRESSURE MAVlink message.
	 * Message specification: http://mavlink.org/messages/common/#SCALED_PRESSURE
	 * @param[in] msg	Received Mavlink msg
	 * @param[in,out] press	SCALED_PRESSURE msg
	 */
	void handle_scaled_pressure(const mavlink::mavlink_message_t *msg, mavlink::common::msg::SCALED_PRESSURE &press)
	{
		if (has_hr_imu)
			return;

		auto header = m_uas->synchronized_header(frame_id, press.time_boot_ms);

		auto temp_msg = boost::make_shared<sensor_msgs::Temperature>();
		temp_msg->header = header;
		temp_msg->temperature = press.temperature / 100.0;
		temp_pub.publish(temp_msg);

		auto atmp_msg = boost::make_shared<sensor_msgs::FluidPressure>();
		atmp_msg->header = header;
		atmp_msg->fluid_pressure = press.press_abs * 100.0;
		press_pub.publish(atmp_msg);
	}

	/**
	 * @brief Checks for connection and overrides variable values
	 * @param[in] connected	Is connected?
	 */
	void connection_cb(bool connected) override
	{
		has_hr_imu = false;
		has_scaled_imu = false;
		has_att_quat = false;
	}
};
}	// namespace std_plugins
}	// namespace mavros

#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS(mavros::std_plugins::IMUPlugin, mavros::plugin::PluginBase)