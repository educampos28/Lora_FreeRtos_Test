/*
 * Euler.h
 *
 *  Created on: 1 de ago de 2019
 *      Author: educampos
 */

#ifndef EULER_H_
#define EULER_H_

#include <math.h>
#include "Math/Matrix.h"


extern void q_123(Matrix& q , float* roll, float* pitch, float* yaw){

	auto q1=q.data[0];
	auto q2=q.data[1];
	auto q3=q.data[2];
	auto q0=q.data[3];


	*roll = atan2((2*(q1*q0+q2*q3)), -pow(q1,2) + pow(q0,2) + pow(q3,2) - pow(q2,2));
	*pitch = -asin(2*(-q0*q2+q1*q3));
	*yaw = atan2((2*(q0*q3+q1*q2)), pow(q1,2) + pow(q0,2) - pow(q3,2) - pow(q2,2));
}


extern void q_321(Matrix& q , float* roll, float* pitch, float* yaw){

	auto q1=q.data[0];
	auto q2=q.data[1];
	auto q3=q.data[2];
	auto q0=q.data[3];


    *roll=atan2((2*(q0*q3-q1*q2)), pow(q1,2) + pow(q0,2) - pow(q3,2) - pow(q2,2));
    *pitch = asin(2*(q0*q2+q1*q3));
    *yaw = atan2((2*(q1*q0-q2*q3)), -pow(q1,2) + pow(q0,2) + pow(q3,2) - pow(q2,2));
}



#endif /* EULER_H_ */
