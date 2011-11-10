/*%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

 Module:       FGPropagate.cpp
 Author:       Jon S. Berndt
 Date started: 01/05/99
 Purpose:      Integrate the EOM to determine instantaneous position
 Called by:    FGFDMExec

 ------------- Copyright (C) 1999  Jon S. Berndt (jon@jsbsim.org) -------------

 This program is free software; you can redistribute it and/or modify it under
 the terms of the GNU Lesser General Public License as published by the Free Software
 Foundation; either version 2 of the License, or (at your option) any later
 version.

 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 details.

 You should have received a copy of the GNU Lesser General Public License along with
 this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 Place - Suite 330, Boston, MA  02111-1307, USA.

 Further information about the GNU Lesser General Public License can also be found on
 the world wide web at http://www.gnu.org.

FUNCTIONAL DESCRIPTION
--------------------------------------------------------------------------------
This class encapsulates the integration of rates and accelerations to get the
current position of the aircraft.

HISTORY
--------------------------------------------------------------------------------
01/05/99   JSB   Created

%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
COMMENTS, REFERENCES,  and NOTES
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
[1] Cooke, Zyda, Pratt, and McGhee, "NPSNET: Flight Simulation Dynamic Modeling
    Using Quaternions", Presence, Vol. 1, No. 4, pp. 404-420  Naval Postgraduate
    School, January 1994
[2] D. M. Henderson, "Euler Angles, Quaternions, and Transformation Matrices",
    JSC 12960, July 1977
[3] Richard E. McFarland, "A Standard Kinematic Model for Flight Simulation at
    NASA-Ames", NASA CR-2497, January 1975
[4] Barnes W. McCormick, "Aerodynamics, Aeronautics, and Flight Mechanics",
    Wiley & Sons, 1979 ISBN 0-471-03032-5
[5] Bernard Etkin, "Dynamics of Flight, Stability and Control", Wiley & Sons,
    1982 ISBN 0-471-08936-2
[6] Erin Catto, "Iterative Dynamics with Temporal Coherence", February 22, 2005

%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
INCLUDES
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%*/

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <iomanip>

#include "FGPropagate.h"
#include "FGGroundReactions.h"
#include "FGFDMExec.h"
#include "FGAircraft.h"
#include "FGMassBalance.h"
#include "FGInertial.h"
#include "input_output/FGPropertyManager.h"

using namespace std;

namespace JSBSim {

static const char *IdSrc = "$Id: FGPropagate.cpp,v 1.88 2011/05/20 03:18:36 jberndt Exp $";
static const char *IdHdr = ID_PROPAGATE;

/*%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
CLASS IMPLEMENTATION
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%*/

FGPropagate::FGPropagate(FGFDMExec* fdmex)
  : FGModel(fdmex),
    LocalTerrainRadius(0),
    SeaLevelRadius(0),
    VehicleRadius(0)
{
  Debug(0);
  Name = "FGPropagate";
  gravType = gtWGS84;
 
  vPQRidot.InitMatrix();
  vQtrndot = FGQuaternion(0,0,0);
  vUVWidot.InitMatrix();
  vInertialVelocity.InitMatrix();

  /// These define the indices use to select the various integrators.
  // eNone = 0, eRectEuler, eTrapezoidal, eAdamsBashforth2, eAdamsBashforth3, eAdamsBashforth4};

  integrator_rotational_rate = eRectEuler;
  integrator_translational_rate = eAdamsBashforth2;
  integrator_rotational_position = eRectEuler;
  integrator_translational_position = eAdamsBashforth3;

  VState.dqPQRidot.resize(4, FGColumnVector3(0.0,0.0,0.0));
  VState.dqUVWidot.resize(4, FGColumnVector3(0.0,0.0,0.0));
  VState.dqInertialVelocity.resize(4, FGColumnVector3(0.0,0.0,0.0));
  VState.dqQtrndot.resize(4, FGQuaternion(0.0,0.0,0.0));

  bind();
  Debug(0);
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

FGPropagate::~FGPropagate(void)
{
  Debug(1);
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

bool FGPropagate::InitModel(void)
{
  // For initialization ONLY:
  SeaLevelRadius = LocalTerrainRadius = FDMExec->GetInertial()->GetRefRadius();

  VState.vLocation.SetRadius( LocalTerrainRadius + 4.0 );
  VState.vLocation.SetEllipse(FDMExec->GetInertial()->GetSemimajor(), FDMExec->GetInertial()->GetSemiminor());
  vOmegaEarth = FGColumnVector3( 0.0, 0.0, FDMExec->GetInertial()->omega() ); // Earth rotation vector

  vPQRidot.InitMatrix();
  vQtrndot = FGQuaternion(0,0,0);
  vUVWidot.InitMatrix();
  vInertialVelocity.InitMatrix();

  VState.dqPQRidot.resize(4, FGColumnVector3(0.0,0.0,0.0));
  VState.dqUVWidot.resize(4, FGColumnVector3(0.0,0.0,0.0));
  VState.dqInertialVelocity.resize(4, FGColumnVector3(0.0,0.0,0.0));
  VState.dqQtrndot.resize(4, FGColumnVector3(0.0,0.0,0.0));

  integrator_rotational_rate = eRectEuler;
  integrator_translational_rate = eAdamsBashforth2;
  integrator_rotational_position = eRectEuler;
  integrator_translational_position = eAdamsBashforth3;

  return true;
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

void FGPropagate::SetInitialState(const FGInitialCondition *FGIC)
{
  SetSeaLevelRadius(FGIC->GetSeaLevelRadiusFtIC());
  SetTerrainElevation(FGIC->GetTerrainElevationFtIC());

  // Initialize the State Vector elements and the transformation matrices

  // Set the position lat/lon/radius
  VState.vLocation.SetPosition( FGIC->GetLongitudeRadIC(),
                                FGIC->GetLatitudeRadIC(),
                                FGIC->GetAltitudeASLFtIC() + FGIC->GetSeaLevelRadiusFtIC() );

  VState.vLocation.SetEarthPositionAngle(FDMExec->GetInertial()->GetEarthPositionAngle());

  Ti2ec = VState.vLocation.GetTi2ec(); // ECI to ECEF transform
  Tec2i = Ti2ec.Transposed();          // ECEF to ECI frame transform

  VState.vInertialPosition = Tec2i * VState.vLocation;

  UpdateLocationMatrices();

  // Set the orientation from the euler angles (is normalized within the
  // constructor). The Euler angles represent the orientation of the body
  // frame relative to the local frame.
  VState.qAttitudeLocal = FGQuaternion( FGIC->GetPhiRadIC(),
                                        FGIC->GetThetaRadIC(),
                                        FGIC->GetPsiRadIC() );

  VState.qAttitudeECI = Ti2l.GetQuaternion()*VState.qAttitudeLocal;
  UpdateBodyMatrices();

  // Set the velocities in the instantaneus body frame
  VState.vUVW = FGColumnVector3( FGIC->GetUBodyFpsIC(),
                                 FGIC->GetVBodyFpsIC(),
                                 FGIC->GetWBodyFpsIC() );

  // Compute the local frame ECEF velocity
  vVel = Tb2l * VState.vUVW;

  // Recompute the LocalTerrainRadius.
  RecomputeLocalTerrainRadius();

  VehicleRadius = GetRadius();

  // Set the angular velocities of the body frame relative to the ECEF frame,
  // expressed in the body frame.
  VState.vPQR = FGColumnVector3( FGIC->GetPRadpsIC(),
                                 FGIC->GetQRadpsIC(),
                                 FGIC->GetRRadpsIC() );

  VState.vPQRi = VState.vPQR + Ti2b * vOmegaEarth;

  // Make an initial run and set past values
  InitializeDerivatives();
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
/*
Purpose: Called on a schedule to perform EOM integration
Notes:   [JB] Run in standalone mode, SeaLevelRadius will be reference radius.
         In FGFS, SeaLevelRadius is stuffed from FGJSBSim in JSBSim.cxx each pass.

At the top of this Run() function, see several "shortcuts" (or, aliases) being
set up for use later, rather than using the longer class->function() notation.

This propagation is done using the current state values
and current derivatives. Based on these values we compute an approximation to the
state values for (now + dt).

In the code below, variables named beginning with a small "v" refer to a 
a column vector, variables named beginning with a "T" refer to a transformation
matrix. ECEF refers to Earth Centered Earth Fixed. ECI refers to Earth Centered
Inertial.

*/

bool FGPropagate::Run(bool Holding)
{
  if (FGModel::Run(Holding)) return true;  // Fast return if we have nothing to do ...
  if (Holding) return false;

  double dt = FDMExec->GetDeltaT()*rate;  // The 'stepsize'

  RunPreFunctions();

  // Calculate state derivatives
  CalculatePQRdot();           // Angular rate derivative
  CalculateUVWdot();           // Translational rate derivative
  ResolveFrictionForces(dt);   // Update rate derivatives with friction forces
  CalculateQuatdot();          // Angular orientation derivative

  // Propagate rotational / translational velocity, angular /translational position, respectively.

  Integrate(VState.vPQRi,             vPQRidot,          VState.dqPQRidot,          dt, integrator_rotational_rate);
  Integrate(VState.qAttitudeECI,      vQtrndot,          VState.dqQtrndot,          dt, integrator_rotational_position);
  Integrate(VState.vInertialPosition, VState.vInertialVelocity, VState.dqInertialVelocity, dt, integrator_translational_position);
  Integrate(VState.vInertialVelocity, vUVWidot,          VState.dqUVWidot,          dt, integrator_translational_rate);

  // CAUTION : the order of the operations below is very important to get transformation
  // matrices that are consistent with the new state of the vehicle

  // 1. Update the Earth position angle (EPA)
  VState.vLocation.SetEarthPositionAngle(FDMExec->GetInertial()->GetEarthPositionAngle());

  // 2. Update the Ti2ec and Tec2i transforms from the updated EPA
  Ti2ec = VState.vLocation.GetTi2ec(); // ECI to ECEF transform
  Tec2i = Ti2ec.Transposed();          // ECEF to ECI frame transform

  // 3. Update the location from the updated Ti2ec and inertial position
  VState.vLocation = Ti2ec*VState.vInertialPosition;

  // 4. Update the other "Location-based" transformation matrices from the updated
  //    vLocation vector.
  UpdateLocationMatrices();

  // 5. Normalize the ECI Attitude quaternion
  VState.qAttitudeECI.Normalize();

  // 6. Update the "Orientation-based" transformation matrices from the updated 
  //    orientation quaternion and vLocation vector.
  UpdateBodyMatrices();

  // Translational position derivative (velocities are integrated in the inertial frame)
  CalculateUVW();

  // Set auxilliary state variables
  RecomputeLocalTerrainRadius();

  VehicleRadius = GetRadius(); // Calculate current aircraft radius from center of planet

  VState.vPQR = VState.vPQRi - Ti2b * vOmegaEarth;

  VState.qAttitudeLocal = Tl2b.GetQuaternion();

  // Compute vehicle velocity wrt ECEF frame, expressed in Local horizontal frame.
  vVel = Tb2l * VState.vUVW;

  RunPostFunctions();

  Debug(2);
  return false;
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// Compute body frame rotational accelerations based on the current body moments
//
// vPQRdot is the derivative of the absolute angular velocity of the vehicle 
// (body rate with respect to the inertial frame), expressed in the body frame,
// where the derivative is taken in the body frame.
// J is the inertia matrix
// Jinv is the inverse inertia matrix
// vMoments is the moment vector in the body frame
// VState.vPQRi is the total inertial angular velocity of the vehicle
// expressed in the body frame.
// Reference: See Stevens and Lewis, "Aircraft Control and Simulation", 
//            Second edition (2004), eqn 1.5-16e (page 50)

void FGPropagate::CalculatePQRdot(void)
{
  const FGColumnVector3& vMoments = FDMExec->GetAircraft()->GetMoments(); // current moments
  const FGMatrix33& J = FDMExec->GetMassBalance()->GetJ();                // inertia matrix
  const FGMatrix33& Jinv = FDMExec->GetMassBalance()->GetJinv();          // inertia matrix inverse

  // Compute body frame rotational accelerations based on the current body
  // moments and the total inertial angular velocity expressed in the body
  // frame.

  vPQRidot = Jinv*(vMoments - VState.vPQRi*(J*VState.vPQRi));
  vPQRdot = vPQRidot - VState.vPQRi * (Ti2b * vOmegaEarth);
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// Compute the quaternion orientation derivative
//
// vQtrndot is the quaternion derivative.
// Reference: See Stevens and Lewis, "Aircraft Control and Simulation", 
//            Second edition (2004), eqn 1.5-16b (page 50)

void FGPropagate::CalculateQuatdot(void)
{
  // Compute quaternion orientation derivative on current body rates
  vQtrndot = VState.qAttitudeECI.GetQDot( VState.vPQRi);
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// This set of calculations results in the body and inertial frame accelerations
// being computed.
// Compute body and inertial frames accelerations based on the current body
// forces including centripetal and coriolis accelerations for the former.
// vOmegaEarth is the Earth angular rate - expressed in the inertial frame -
//   so it has to be transformed to the body frame. More completely,
//   vOmegaEarth is the rate of the ECEF frame relative to the Inertial
//   frame (ECI), expressed in the Inertial frame.
// vForces is the total force on the vehicle in the body frame.
// VState.vPQR is the vehicle body rate relative to the ECEF frame, expressed
//   in the body frame.
// VState.vUVW is the vehicle velocity relative to the ECEF frame, expressed
//   in the body frame.
// Reference: See Stevens and Lewis, "Aircraft Control and Simulation", 
//            Second edition (2004), eqns 1.5-13 (pg 48) and 1.5-16d (page 50)

void FGPropagate::CalculateUVWdot(void)
{
  double mass = FDMExec->GetMassBalance()->GetMass();                      // mass
  const FGColumnVector3& vForces = FDMExec->GetAircraft()->GetForces();    // current forces

  vUVWdot = vForces/mass - (VState.vPQR + 2.0*(Ti2b *vOmegaEarth)) * VState.vUVW;

  // Include Centripetal acceleration.
  vUVWdot -= Ti2b * (vOmegaEarth*(vOmegaEarth*VState.vInertialPosition));

  // Include Gravitation accel
  switch (gravType) {
    case gtStandard:
      vGravAccel = Tl2b * FGColumnVector3( 0.0, 0.0, FDMExec->GetInertial()->GetGAccel(VehicleRadius) );
      break;
    case gtWGS84:
      vGravAccel = Tec2b * FDMExec->GetInertial()->GetGravityJ2(VState.vLocation);
      break;
  }

  vUVWdot += vGravAccel;
  vUVWidot = Tb2i * (vForces/mass + vGravAccel);
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
  // Transform the velocity vector of the body relative to the origin (Earth
  // center) to be expressed in the inertial frame, and add the vehicle velocity
  // contribution due to the rotation of the planet.
  // Reference: See Stevens and Lewis, "Aircraft Control and Simulation", 
  //            Second edition (2004), eqn 1.5-16c (page 50)

void FGPropagate::CalculateInertialVelocity(void)
{
  VState.vInertialVelocity = Tb2i * VState.vUVW + (vOmegaEarth * VState.vInertialPosition);
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
  // Transform the velocity vector of the inertial frame to be expressed in the
  // body frame relative to the origin (Earth center), and substract the vehicle
  // velocity contribution due to the rotation of the planet.

void FGPropagate::CalculateUVW(void)
{
  VState.vUVW = Ti2b * (VState.vInertialVelocity - (vOmegaEarth * VState.vInertialPosition));
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

void FGPropagate::Integrate( FGColumnVector3& Integrand,
                             FGColumnVector3& Val,
                             deque <FGColumnVector3>& ValDot,
                             double dt,
                             eIntegrateType integration_type)
{
  ValDot.push_front(Val);
  ValDot.pop_back();

  switch(integration_type) {
  case eRectEuler:       Integrand += dt*ValDot[0];
    break;
  case eTrapezoidal:     Integrand += 0.5*dt*(ValDot[0] + ValDot[1]);
    break;
  case eAdamsBashforth2: Integrand += dt*(1.5*ValDot[0] - 0.5*ValDot[1]);
    break;
  case eAdamsBashforth3: Integrand += (1/12.0)*dt*(23.0*ValDot[0] - 16.0*ValDot[1] + 5.0*ValDot[2]);
    break;
  case eAdamsBashforth4: Integrand += (1/24.0)*dt*(55.0*ValDot[0] - 59.0*ValDot[1] + 37.0*ValDot[2] - 9.0*ValDot[3]);
    break;
  case eNone: // do nothing, freeze translational rate
    break;
  }
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

void FGPropagate::Integrate( FGQuaternion& Integrand,
                             FGQuaternion& Val,
                             deque <FGQuaternion>& ValDot,
                             double dt,
                             eIntegrateType integration_type)
{
  ValDot.push_front(Val);
  ValDot.pop_back();

  switch(integration_type) {
  case eRectEuler:       Integrand += dt*ValDot[0];
    break;
  case eTrapezoidal:     Integrand += 0.5*dt*(ValDot[0] + ValDot[1]);
    break;
  case eAdamsBashforth2: Integrand += dt*(1.5*ValDot[0] - 0.5*ValDot[1]);
    break;
  case eAdamsBashforth3: Integrand += (1/12.0)*dt*(23.0*ValDot[0] - 16.0*ValDot[1] + 5.0*ValDot[2]);
    break;
  case eAdamsBashforth4: Integrand += (1/24.0)*dt*(55.0*ValDot[0] - 59.0*ValDot[1] + 37.0*ValDot[2] - 9.0*ValDot[3]);
    break;
  case eNone: // do nothing, freeze rotational rate
    break;
  }
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// Evaluates the rates (translation or rotation) that the friction forces have
// to resist to. This includes the external forces and moments as well as the
// relative movement between the aircraft and the ground.
// Erin Catto's paper (see ref [6]) only supports Euler integration scheme and
// this algorithm has been adapted to handle the multistep algorithms that
// JSBSim supports (i.e. Trapezoidal, Adams-Bashforth 2, 3 and 4). The capacity
// to handle the multistep integration schemes adds some complexity but it
// significantly helps stabilizing the friction forces.

void FGPropagate::EvaluateRateToResistTo(FGColumnVector3& vdot,
                                         const FGColumnVector3& Val,
                                         const FGColumnVector3& ValDot,
                                         const FGColumnVector3& LocalTerrainVal,
                                         deque <FGColumnVector3>& dqValDot,
                                         const double dt,
                                         const eIntegrateType integration_type)
{
  switch(integration_type) {
  case eAdamsBashforth4:
    vdot = ValDot + Ti2b * (-59.*dqValDot[0]+37.*dqValDot[1]-9.*dqValDot[2])/55.;
    if (dt > 0.) // Zeroes out the relative movement between aircraft and ground
      vdot += 24.*(Val - Tec2b * LocalTerrainVal) / (55.*dt);
    break;
  case eAdamsBashforth3:
    vdot = ValDot + Ti2b * (-16.*dqValDot[0]+5.*dqValDot[1])/23.;
    if (dt > 0.) // Zeroes out the relative movement between aircraft and ground
      vdot += 12.*(Val - Tec2b * LocalTerrainVal) / (23.*dt);
    break;
  case eAdamsBashforth2:
    vdot = ValDot - Ti2b * dqValDot[0]/3.;
    if (dt > 0.) // Zeroes out the relative movement between aircraft and ground
      vdot += 2.*(Val - Tec2b * LocalTerrainVal) / (3.*dt);
    break;
  case eTrapezoidal:
    vdot = ValDot + Ti2b * dqValDot[0];
    if (dt > 0.) // Zeroes out the relative movement between aircraft and ground
      vdot += 2.*(Val - Tec2b * LocalTerrainVal) / dt;
    break;
  case eRectEuler:
    vdot = ValDot;
    if (dt > 0.) // Zeroes out the relative movement between aircraft and ground
      vdot += (Val - Tec2b * LocalTerrainVal) / dt;
    break;
  case eNone:
    break;
  }
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// Resolves the contact forces just before integrating the EOM.
// This routine is using Lagrange multipliers and the projected Gauss-Seidel
// (PGS) method.
// Reference: See Erin Catto, "Iterative Dynamics with Temporal Coherence", 
//            February 22, 2005
// In JSBSim there is only one rigid body (the aircraft) and there can be
// multiple points of contact between the aircraft and the ground. As a
// consequence our matrix J*M^-1*J^T is not sparse and the algorithm described
// in Catto's paper has been adapted accordingly.
// The friction forces are resolved in the body frame relative to the origin
// (Earth center).

void FGPropagate::ResolveFrictionForces(double dt)
{
  const double invMass = 1.0 / FDMExec->GetMassBalance()->GetMass();
  const FGMatrix33& Jinv = FDMExec->GetMassBalance()->GetJinv();
  vector <FGColumnVector3> JacF, JacM;
  vector<double> lambda, lambdaMin, lambdaMax;
  FGColumnVector3 vdot, wdot;
  FGColumnVector3 Fc, Mc;
  int n = 0;

  // Compiles data from the ground reactions to build up the jacobian matrix
  for (MultiplierIterator it=MultiplierIterator(FDMExec->GetGroundReactions()); *it; ++it, n++) {
    JacF.push_back((*it)->ForceJacobian);
    JacM.push_back((*it)->MomentJacobian);
    lambda.push_back((*it)->value);
    lambdaMax.push_back((*it)->Max);
    lambdaMin.push_back((*it)->Min);
  }

  // If no gears are in contact with the ground then return
  if (!n) return;

  vector<double> a(n*n); // Will contain J*M^-1*J^T
  vector<double> rhs(n);

  // Assemble the linear system of equations
  for (int i=0; i < n; i++) {
    for (int j=0; j < i; j++)
      a[i*n+j] = a[j*n+i]; // Takes advantage of the symmetry of J^T*M^-1*J
    for (int j=i; j < n; j++)
      a[i*n+j] = DotProduct(JacF[i],invMass*JacF[j])+DotProduct(JacM[i],Jinv*JacM[j]);
  }

  // Assemble the RHS member

  // Translation
  EvaluateRateToResistTo(vdot, VState.vUVW, vUVWdot, LocalTerrainVelocity,
                         VState.dqUVWidot, dt, integrator_translational_rate);

  // Rotation
  EvaluateRateToResistTo(wdot, VState.vPQR, vPQRdot, LocalTerrainAngularVelocity,
                         VState.dqPQRidot, dt, integrator_rotational_rate);

  // Prepare the linear system for the Gauss-Seidel algorithm :
  // 1. Compute the right hand side member 'rhs'
  // 2. Divide every line of 'a' and 'rhs' by a[i,i]. This is in order to save
  //    a division computation at each iteration of Gauss-Seidel.
  for (int i=0; i < n; i++) {
    double d = 1.0 / a[i*n+i];

    rhs[i] = -(DotProduct(JacF[i],vdot)+DotProduct(JacM[i],wdot))*d;
    for (int j=0; j < n; j++)
      a[i*n+j] *= d;
  }

  // Resolve the Lagrange multipliers with the projected Gauss-Seidel method
  for (int iter=0; iter < 50; iter++) {
    double norm = 0.;

    for (int i=0; i < n; i++) {
      double lambda0 = lambda[i];
      double dlambda = rhs[i];
      
      for (int j=0; j < n; j++)
        dlambda -= a[i*n+j]*lambda[j];

      lambda[i] = Constrain(lambdaMin[i], lambda0+dlambda, lambdaMax[i]);
      dlambda = lambda[i] - lambda0;

      norm += fabs(dlambda);
    }

    if (norm < 1E-5) break;
  }

  // Calculate the total friction forces and moments

  Fc.InitMatrix();
  Mc.InitMatrix();

  for (int i=0; i< n; i++) {
    Fc += lambda[i]*JacF[i];
    Mc += lambda[i]*JacM[i];
  }

  vUVWdot += invMass * Fc;
  vUVWidot += invMass * Tb2i * Fc;
  vPQRdot += Jinv * Mc;
  vPQRidot += Jinv * Mc;

  // Save the value of the Lagrange multipliers to accelerate the convergence
  // of the Gauss-Seidel algorithm at next iteration.
  int i = 0;
  for (MultiplierIterator it=MultiplierIterator(FDMExec->GetGroundReactions()); *it; ++it)
    (*it)->value = lambda[i++];

  FDMExec->GetGroundReactions()->UpdateForcesAndMoments();
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

void FGPropagate::UpdateLocationMatrices(void)
{
  Tl2ec = VState.vLocation.GetTl2ec(); // local to ECEF transform
  Tec2l = Tl2ec.Transposed();          // ECEF to local frame transform
  Ti2l  = VState.vLocation.GetTi2l();  // ECI to local frame transform
  Tl2i  = Ti2l.Transposed();           // local to ECI transform
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

void FGPropagate::UpdateBodyMatrices(void)
{
  Ti2b  = VState.qAttitudeECI.GetT(); // ECI to body frame transform
  Tb2i  = Ti2b.Transposed();          // body to ECI frame transform
  Tl2b  = Ti2b * Tl2i;                // local to body frame transform
  Tb2l  = Tl2b.Transposed();          // body to local frame transform
  Tec2b = Ti2b * Tec2i;               // ECEF to body frame transform
  Tb2ec = Tec2b.Transposed();         // body to ECEF frame tranform
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

void FGPropagate::SetInertialOrientation(FGQuaternion Qi) {
  VState.qAttitudeECI = Qi;
  VState.qAttitudeECI.Normalize();
  UpdateBodyMatrices();
  VState.qAttitudeLocal = Tl2b.GetQuaternion();
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

void FGPropagate::SetInertialVelocity(FGColumnVector3 Vi) {
  VState.vInertialVelocity = Vi;
  CalculateUVW();
  vVel = Tb2l * VState.vUVW;
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

void FGPropagate::SetInertialRates(FGColumnVector3 vRates) {
  VState.vPQRi = Ti2b * vRates;
  VState.vPQR = VState.vPQRi - Ti2b * vOmegaEarth;
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

void FGPropagate::InitializeDerivatives(void)
{
  // Make an initial run and set past values
  CalculatePQRdot();           // Angular rate derivative
  CalculateUVWdot();           // Translational rate derivative
  ResolveFrictionForces(0.);   // Update rate derivatives with friction forces
  CalculateQuatdot();          // Angular orientation derivative
  CalculateInertialVelocity(); // Translational position derivative

  // Initialize past values deques
  VState.dqPQRidot.clear();
  VState.dqUVWidot.clear();
  VState.dqInertialVelocity.clear();
  VState.dqQtrndot.clear();
  for (int i=0; i<4; i++) {
    VState.dqPQRidot.push_front(vPQRidot);
    VState.dqUVWidot.push_front(vUVWidot);
    VState.dqInertialVelocity.push_front(VState.vInertialVelocity);
    VState.dqQtrndot.push_front(vQtrndot);
  }
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

void FGPropagate::RecomputeLocalTerrainRadius(void)
{
  FGLocation contactloc;
  FGColumnVector3 dv;
  double t = FDMExec->GetSimTime();

  // Get the LocalTerrain radius.
  FDMExec->GetGroundCallback()->GetAGLevel(t, VState.vLocation, contactloc, dv,
                                           LocalTerrainVelocity, LocalTerrainAngularVelocity);
  LocalTerrainRadius = contactloc.GetRadius(); 
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

void FGPropagate::SetTerrainElevation(double terrainElev)
{
  LocalTerrainRadius = terrainElev + SeaLevelRadius;
  FDMExec->GetGroundCallback()->SetTerrainGeoCentRadius(LocalTerrainRadius);
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

double FGPropagate::GetTerrainElevation(void) const
{
  return FDMExec->GetGroundCallback()->GetTerrainGeoCentRadius()-SeaLevelRadius;
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

double FGPropagate::GetDistanceAGL(void) const
{
  return VState.vLocation.GetRadius() - LocalTerrainRadius;
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

void FGPropagate::SetVState(const VehicleState& vstate)
{
  VState.vLocation = vstate.vLocation;
  VState.vLocation.SetEarthPositionAngle(FDMExec->GetInertial()->GetEarthPositionAngle());
  Ti2ec = VState.vLocation.GetTi2ec(); // useless ?
  Tec2i = Ti2ec.Transposed();
  UpdateLocationMatrices();
  SetInertialOrientation(vstate.qAttitudeECI);
  RecomputeLocalTerrainRadius();
  VehicleRadius = GetRadius();
  VState.vUVW = vstate.vUVW;
  vVel = Tb2l * VState.vUVW;
  VState.vPQR = vstate.vPQR;
  VState.vPQRi = VState.vPQR + Ti2b * vOmegaEarth;
  VState.vInertialPosition = vstate.vInertialPosition;

  InitializeDerivatives();
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

void FGPropagate::UpdateVehicleState(void)
{
  RecomputeLocalTerrainRadius();
  VehicleRadius = GetRadius();
  VState.vInertialPosition = Tec2i * VState.vLocation;
  UpdateLocationMatrices();
  UpdateBodyMatrices();
  vVel = Tb2l * VState.vUVW;
  VState.qAttitudeLocal = Tl2b.GetQuaternion();
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

void FGPropagate::SetLocation(const FGLocation& l)
{
  VState.vLocation = l;
  VState.vLocation.SetEarthPositionAngle(FDMExec->GetInertial()->GetEarthPositionAngle());
  Ti2ec = VState.vLocation.GetTi2ec(); // useless ?
  Tec2i = Ti2ec.Transposed();
  UpdateVehicleState();
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

void FGPropagate::DumpState(void)
{
  cout << endl;
  cout << fgblue
       << "------------------------------------------------------------------" << reset << endl;
  cout << highint
       << "State Report at sim time: " << FDMExec->GetSimTime() << " seconds" << reset << endl;
  cout << "  " << underon
       <<   "Position" << underoff << endl;
  cout << "    ECI:   " << VState.vInertialPosition.Dump(", ") << " (x,y,z, in ft)" << endl;
  cout << "    ECEF:  " << VState.vLocation << " (x,y,z, in ft)"  << endl;
  cout << "    Local: " << VState.vLocation.GetLatitudeDeg()
                        << ", " << VState.vLocation.GetLongitudeDeg()
                        << ", " << GetAltitudeASL() << " (lat, lon, alt in deg and ft)" << endl;

  cout << endl << "  " << underon
       <<   "Orientation" << underoff << endl;
  cout << "    ECI:   " << VState.qAttitudeECI.GetEulerDeg().Dump(", ") << " (phi, theta, psi in deg)" << endl;
  cout << "    Local: " << VState.qAttitudeLocal.GetEulerDeg().Dump(", ") << " (phi, theta, psi in deg)" << endl;

  cout << endl << "  " << underon
       <<   "Velocity" << underoff << endl;
  cout << "    ECI:   " << VState.vInertialVelocity.Dump(", ") << " (x,y,z in ft/s)" << endl;
  cout << "    ECEF:  " << (Tb2ec * VState.vUVW).Dump(", ")  << " (x,y,z in ft/s)"  << endl;
  cout << "    Local: " << GetVel() << " (n,e,d in ft/sec)" << endl;
  cout << "    Body:  " << GetUVW() << " (u,v,w in ft/sec)" << endl;

  cout << endl << "  " << underon
       <<   "Body Rates (relative to given frame, expressed in body frame)" << underoff << endl;
  cout << "    ECI:   " << (VState.vPQRi*radtodeg).Dump(", ") << " (p,q,r in deg/s)" << endl;
  cout << "    ECEF:  " << (VState.vPQR*radtodeg).Dump(", ") << " (p,q,r in deg/s)" << endl;
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

void FGPropagate::bind(void)
{
  typedef double (FGPropagate::*PMF)(int) const;

  PropertyManager->Tie("velocities/h-dot-fps", this, &FGPropagate::Gethdot);

  PropertyManager->Tie("velocities/v-north-fps", this, eNorth, (PMF)&FGPropagate::GetVel);
  PropertyManager->Tie("velocities/v-east-fps", this, eEast, (PMF)&FGPropagate::GetVel);
  PropertyManager->Tie("velocities/v-down-fps", this, eDown, (PMF)&FGPropagate::GetVel);

  PropertyManager->Tie("velocities/u-fps", this, eU, (PMF)&FGPropagate::GetUVW);
  PropertyManager->Tie("velocities/v-fps", this, eV, (PMF)&FGPropagate::GetUVW);
  PropertyManager->Tie("velocities/w-fps", this, eW, (PMF)&FGPropagate::GetUVW);

  PropertyManager->Tie("velocities/p-rad_sec", this, eP, (PMF)&FGPropagate::GetPQR);
  PropertyManager->Tie("velocities/q-rad_sec", this, eQ, (PMF)&FGPropagate::GetPQR);
  PropertyManager->Tie("velocities/r-rad_sec", this, eR, (PMF)&FGPropagate::GetPQR);

  PropertyManager->Tie("velocities/pi-rad_sec", this, eP, (PMF)&FGPropagate::GetPQRi);
  PropertyManager->Tie("velocities/qi-rad_sec", this, eQ, (PMF)&FGPropagate::GetPQRi);
  PropertyManager->Tie("velocities/ri-rad_sec", this, eR, (PMF)&FGPropagate::GetPQRi);

  PropertyManager->Tie("velocities/eci-velocity-mag-fps", this, &FGPropagate::GetInertialVelocityMagnitude);

  PropertyManager->Tie("accelerations/pdot-rad_sec2", this, eP, (PMF)&FGPropagate::GetPQRdot);
  PropertyManager->Tie("accelerations/qdot-rad_sec2", this, eQ, (PMF)&FGPropagate::GetPQRdot);
  PropertyManager->Tie("accelerations/rdot-rad_sec2", this, eR, (PMF)&FGPropagate::GetPQRdot);

  PropertyManager->Tie("accelerations/udot-ft_sec2", this, eU, (PMF)&FGPropagate::GetUVWdot);
  PropertyManager->Tie("accelerations/vdot-ft_sec2", this, eV, (PMF)&FGPropagate::GetUVWdot);
  PropertyManager->Tie("accelerations/wdot-ft_sec2", this, eW, (PMF)&FGPropagate::GetUVWdot);

  PropertyManager->Tie("position/h-sl-ft", this, &FGPropagate::GetAltitudeASL, &FGPropagate::SetAltitudeASL, true);
  PropertyManager->Tie("position/h-sl-meters", this, &FGPropagate::GetAltitudeASLmeters, &FGPropagate::SetAltitudeASLmeters, true);
  PropertyManager->Tie("position/lat-gc-rad", this, &FGPropagate::GetLatitude, &FGPropagate::SetLatitude);
  PropertyManager->Tie("position/long-gc-rad", this, &FGPropagate::GetLongitude, &FGPropagate::SetLongitude);
  PropertyManager->Tie("position/lat-gc-deg", this, &FGPropagate::GetLatitudeDeg, &FGPropagate::SetLatitudeDeg);
  PropertyManager->Tie("position/long-gc-deg", this, &FGPropagate::GetLongitudeDeg, &FGPropagate::SetLongitudeDeg);
  PropertyManager->Tie("position/lat-geod-rad", this, &FGPropagate::GetGeodLatitudeRad);
  PropertyManager->Tie("position/lat-geod-deg", this, &FGPropagate::GetGeodLatitudeDeg);
  PropertyManager->Tie("position/geod-alt-ft", this, &FGPropagate::GetGeodeticAltitude);
  PropertyManager->Tie("position/h-agl-ft", this,  &FGPropagate::GetDistanceAGL, &FGPropagate::SetDistanceAGL);
  PropertyManager->Tie("position/radius-to-vehicle-ft", this, &FGPropagate::GetRadius);
  PropertyManager->Tie("position/terrain-elevation-asl-ft", this,
                          &FGPropagate::GetTerrainElevation,
                          &FGPropagate::SetTerrainElevation, false);

  PropertyManager->Tie("metrics/terrain-radius", this, &FGPropagate::GetLocalTerrainRadius);

  PropertyManager->Tie("attitude/phi-rad", this, (int)ePhi, (PMF)&FGPropagate::GetEuler);
  PropertyManager->Tie("attitude/theta-rad", this, (int)eTht, (PMF)&FGPropagate::GetEuler);
  PropertyManager->Tie("attitude/psi-rad", this, (int)ePsi, (PMF)&FGPropagate::GetEuler);

  PropertyManager->Tie("attitude/roll-rad", this, (int)ePhi, (PMF)&FGPropagate::GetEuler);
  PropertyManager->Tie("attitude/pitch-rad", this, (int)eTht, (PMF)&FGPropagate::GetEuler);
  PropertyManager->Tie("attitude/heading-true-rad", this, (int)ePsi, (PMF)&FGPropagate::GetEuler);
  
  PropertyManager->Tie("simulation/integrator/rate/rotational", (int*)&integrator_rotational_rate);
  PropertyManager->Tie("simulation/integrator/rate/translational", (int*)&integrator_translational_rate);
  PropertyManager->Tie("simulation/integrator/position/rotational", (int*)&integrator_rotational_position);
  PropertyManager->Tie("simulation/integrator/position/translational", (int*)&integrator_translational_position);
  PropertyManager->Tie("simulation/gravity-model", &gravType);
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
//    The bitmasked value choices are as follows:
//    unset: In this case (the default) JSBSim would only print
//       out the normally expected messages, essentially echoing
//       the config files as they are read. If the environment
//       variable is not set, debug_lvl is set to 1 internally
//    0: This requests JSBSim not to output any messages
//       whatsoever.
//    1: This value explicity requests the normal JSBSim
//       startup messages
//    2: This value asks for a message to be printed out when
//       a class is instantiated
//    4: When this value is set, a message is displayed when a
//       FGModel object executes its Run() method
//    8: When this value is set, various runtime state variables
//       are printed out periodically
//    16: When set various parameters are sanity checked and
//       a message is printed out when they go out of bounds

void FGPropagate::Debug(int from)
{
  if (debug_lvl <= 0) return;

  if (debug_lvl & 1) { // Standard console startup message output
    if (from == 0) { // Constructor

    }
  }
  if (debug_lvl & 2 ) { // Instantiation/Destruction notification
    if (from == 0) cout << "Instantiated: FGPropagate" << endl;
    if (from == 1) cout << "Destroyed:    FGPropagate" << endl;
  }
  if (debug_lvl & 4 ) { // Run() method entry print for FGModel-derived objects
  }
  if (debug_lvl & 8 && from == 2) { // Runtime state variables
    cout << endl << fgblue << highint << left 
         << "  Propagation Report (English units: ft, degrees) at simulation time " << FDMExec->GetSimTime() << " seconds"
         << reset << endl;
    cout << endl;
    cout << highint << "  Earth Position Angle (deg): " << setw(8) << setprecision(3) << reset
                    << FDMExec->GetInertial()->GetEarthPositionAngleDeg() << endl;
    cout << endl;
    cout << highint << "  Body velocity (ft/sec): " << setw(8) << setprecision(3) << reset << VState.vUVW << endl;
    cout << highint << "  Local velocity (ft/sec): " << setw(8) << setprecision(3) << reset << vVel << endl;
    cout << highint << "  Inertial velocity (ft/sec): " << setw(8) << setprecision(3) << reset << VState.vInertialVelocity << endl;
    cout << highint << "  Inertial Position (ft): " << setw(10) << setprecision(3) << reset << VState.vInertialPosition << endl;
    cout << highint << "  Latitude (deg): " << setw(8) << setprecision(3) << reset << VState.vLocation.GetLatitudeDeg() << endl;
    cout << highint << "  Longitude (deg): " << setw(8) << setprecision(3) << reset << VState.vLocation.GetLongitudeDeg() << endl;
    cout << highint << "  Altitude ASL (ft): " << setw(8) << setprecision(3) << reset << GetAltitudeASL() << endl;
    cout << highint << "  Acceleration (NED, ft/sec^2): " << setw(8) << setprecision(3) << reset << Tb2l*GetUVWdot() << endl;
    cout << endl;
    cout << highint << "  Matrix ECEF to Body (Orientation of Body with respect to ECEF): "
                    << reset << endl << Tec2b.Dump("\t", "    ") << endl;
    cout << highint << "    Associated Euler angles (deg): " << setw(8)
                    << setprecision(3) << reset << (Tec2b.GetQuaternion().GetEuler()*radtodeg)
                    << endl << endl;

    cout << highint << "  Matrix Body to ECEF (Orientation of ECEF with respect to Body):"
                    << reset << endl << Tb2ec.Dump("\t", "    ") << endl;
    cout << highint << "    Associated Euler angles (deg): " << setw(8)
                    << setprecision(3) << reset << (Tb2ec.GetQuaternion().GetEuler()*radtodeg)
                    << endl << endl;

    cout << highint << "  Matrix Local to Body (Orientation of Body with respect to Local):"
                    << reset << endl << Tl2b.Dump("\t", "    ") << endl;
    cout << highint << "    Associated Euler angles (deg): " << setw(8)
                    << setprecision(3) << reset << (Tl2b.GetQuaternion().GetEuler()*radtodeg)
                    << endl << endl;

    cout << highint << "  Matrix Body to Local (Orientation of Local with respect to Body):"
                    << reset << endl << Tb2l.Dump("\t", "    ") << endl;
    cout << highint << "    Associated Euler angles (deg): " << setw(8)
                    << setprecision(3) << reset << (Tb2l.GetQuaternion().GetEuler()*radtodeg)
                    << endl << endl;

    cout << highint << "  Matrix Local to ECEF (Orientation of ECEF with respect to Local):"
                    << reset << endl << Tl2ec.Dump("\t", "    ") << endl;
    cout << highint << "    Associated Euler angles (deg): " << setw(8)
                    << setprecision(3) << reset << (Tl2ec.GetQuaternion().GetEuler()*radtodeg)
                    << endl << endl;

    cout << highint << "  Matrix ECEF to Local (Orientation of Local with respect to ECEF):"
                    << reset << endl << Tec2l.Dump("\t", "    ") << endl;
    cout << highint << "    Associated Euler angles (deg): " << setw(8)
                    << setprecision(3) << reset << (Tec2l.GetQuaternion().GetEuler()*radtodeg)
                    << endl << endl;

    cout << highint << "  Matrix ECEF to Inertial (Orientation of Inertial with respect to ECEF):"
                    << reset << endl << Tec2i.Dump("\t", "    ") << endl;
    cout << highint << "    Associated Euler angles (deg): " << setw(8)
                    << setprecision(3) << reset << (Tec2i.GetQuaternion().GetEuler()*radtodeg)
                    << endl << endl;

    cout << highint << "  Matrix Inertial to ECEF (Orientation of ECEF with respect to Inertial):"
                    << reset << endl << Ti2ec.Dump("\t", "    ") << endl;
    cout << highint << "    Associated Euler angles (deg): " << setw(8)
                    << setprecision(3) << reset << (Ti2ec.GetQuaternion().GetEuler()*radtodeg)
                    << endl << endl;

    cout << highint << "  Matrix Inertial to Body (Orientation of Body with respect to Inertial):"
                    << reset << endl << Ti2b.Dump("\t", "    ") << endl;
    cout << highint << "    Associated Euler angles (deg): " << setw(8)
                    << setprecision(3) << reset << (Ti2b.GetQuaternion().GetEuler()*radtodeg)
                    << endl << endl;

    cout << highint << "  Matrix Body to Inertial (Orientation of Inertial with respect to Body):"
                    << reset << endl << Tb2i.Dump("\t", "    ") << endl;
    cout << highint << "    Associated Euler angles (deg): " << setw(8)
                    << setprecision(3) << reset << (Tb2i.GetQuaternion().GetEuler()*radtodeg)
                    << endl << endl;

    cout << highint << "  Matrix Inertial to Local (Orientation of Local with respect to Inertial):"
                    << reset << endl << Ti2l.Dump("\t", "    ") << endl;
    cout << highint << "    Associated Euler angles (deg): " << setw(8)
                    << setprecision(3) << reset << (Ti2l.GetQuaternion().GetEuler()*radtodeg)
                    << endl << endl;

    cout << highint << "  Matrix Local to Inertial (Orientation of Inertial with respect to Local):"
                    << reset << endl << Tl2i.Dump("\t", "    ") << endl;
    cout << highint << "    Associated Euler angles (deg): " << setw(8)
                    << setprecision(3) << reset << (Tl2i.GetQuaternion().GetEuler()*radtodeg)
                    << endl << endl;

    cout << setprecision(6); // reset the output stream
  }
  if (debug_lvl & 16) { // Sanity checking
    if (from == 2) { // State sanity checking
      if (fabs(VState.vPQR.Magnitude()) > 1000.0) {
        cerr << endl << "Vehicle rotation rate is excessive (>1000 rad/sec): " << VState.vPQR.Magnitude() << endl;
        exit(-1);
      }
      if (fabs(VState.vUVW.Magnitude()) > 1.0e10) {
        cerr << endl << "Vehicle velocity is excessive (>1e10 ft/sec): " << VState.vUVW.Magnitude() << endl;
        exit(-1);
      }
      if (fabs(GetDistanceAGL()) > 1e10) {
        cerr << endl << "Vehicle altitude is excessive (>1e10 ft): " << GetDistanceAGL() << endl;
        exit(-1);
      }
    }
  }
  if (debug_lvl & 64) {
    if (from == 0) { // Constructor
      cout << IdSrc << endl;
      cout << IdHdr << endl;
    }
  }
}
}
