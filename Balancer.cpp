#include "Balancer.h"
#include "Gyro.h"
#include "Leg.h"
#include "LegUtil.h"
#include "Debug.h"
#include "Config.h"

#include <Arduino.h>
#include <math.h>

#define MAX_TRIES 12
#define MAX_BAD_DELTAS 8
#define MAX_DELTA_POSITION 0.005
#define BALANCING_UPDATE_INTERVAL 200

unsigned long lastBalancingUpdate;

void Balancer::setup() {
  gyro.setup();

  for (int i = 0; i < MAX_LEGS; i++) {
    legs[i].setup();
  }
}

void Balancer::loop() {
  //DPRINTLN(F("Gyro loop start"));
  gyro.loop();
  //DPRINTLN(F("Gyro loop end"));

  for (int i = 0; i < MAX_LEGS; i++) {
    legs[i].loop();
  }

  switch (state) {
    case State::NoState:
    case State::ZeroState:
    case State::BalancedState:
    case State::ErrorState:
    case State::GroundState:
    case State::FinalState:
      break;
    case State::BalancingState:
      stateBalancingLoop();
      break;
    case State::ToZeroState:
      stateToZeroLoop();
      break;
    case State::ToGroundState:
      stateToGroundLoop();
      break;
    case State::ToFinalState:
      stateToFinalLoop();
      break;
    default:
      DPRINTLN(F("Unknown state."));
  }
}

void Balancer::toZero() {
  setState(State::ToZeroState);
}

void Balancer::toGround() {
  setState(State::ToGroundState);
}

void Balancer::toFinal() {
  setState(State::ToFinalState);
}

void Balancer::balance() {
  balancingAction.tries = 0;
  balancingState = BalancingState::NotBalancing;
  setState(State::BalancingState);
}

void Balancer::forceExpandLeg(int legId) {
  // Same for all states.
  stopAllLegs();
  legs[legId].expand();
  setState(State::NoState);
}

void Balancer::forceCollapseLeg(int legId) {
  // Same for all states.
  setState(State::NoState);
  stopAllLegs();
  legs[legId].collapse();
}

void Balancer::stopAllLegs() {
  setState(State::NoState);
  LegUtil::stopAllMotors(legs);
}

void Balancer::setState(State newState) {
  state = newState;
}

void Balancer::stateToZeroLoop() {
  int c = 0;

  for (int i = 0; i < MAX_LEGS; i++) {
    Leg leg = legs[i];

    if (leg.getPosition() == LegPosition::Zero) {
      c++;
      leg.stop();
//    } else if (leg.getPosition() != LegPosition::Unknown && leg.getPosition() != LegPosition::Zero) {
    } else if (leg.getPosition() != LegPosition::Zero) {
      leg.collapse();
    }
  }

  if (c == MAX_LEGS) {
    setState(State::ZeroState);
  }
}

void Balancer::stateToGroundLoop() {
  int c = 0;
  int isFinalPosition = false;

  for (int i = 0; i < MAX_LEGS; i++) {
    Leg leg = legs[i];

    if (leg.isOnGround()) {
      c++;
      leg.stop();
    } else if (leg.getPosition() == LegPosition::Final) {
      DPRINTLN(F("STOP To Ground operation. Leg at final position detected."));
      isFinalPosition = true;
      break;
    } else {
      leg.expand();
    }
  }

  if (c == MAX_LEGS) {
    setState(State::GroundState);
  }

  if (isFinalPosition) {
    stopAllLegs();
    setState(State::ErrorState);
  }
}

void Balancer::stateToFinalLoop() {
  int c = 0;

  for (int i = 0; i < MAX_LEGS; i++) {
    Leg leg = legs[i];

    if (leg.getPosition() == LegPosition::Final) {
      c++;
      leg.stop();
    } else if (leg.getPosition() != LegPosition::Final) {
      leg.expand();
    }
  }

  if (c == MAX_LEGS) {
    setState(State::ZeroState);
  }
}

void Balancer::stateBalancingLoop() {
  /*
  if (getState() != State::GroundState) {
    DPRINTLN(F("Trailer needs to be on ground!"));
    setState(State::NoState);
    return;
  }
  */

  unsigned long now = millis();

  if (now - lastBalancingUpdate < BALANCING_UPDATE_INTERVAL) {
    return;
  }

  lastBalancingUpdate = millis();

  // trailer can only be balanced if all legs are on ground
  if (gyro.isBalanced()) { // && LegUtil::allLegsOnGround(legs)) {
    DPRINTLN(F("Trailed BALANCED!"));
    LegUtil::stopAllMotors(legs);
    setState(State::BalancedState);
    return;
  }

  // any leg in final position
  if (LegUtil::anyLegInPosition(legs, LegPosition::Final)) {
    DPRINTLN(F("Cannot balance. One of the legs reached final position."));
    LegUtil::stopAllMotors(legs);
    setState(State::ErrorState);
    return;
  }

  if (balancingAction.tries >= MAX_TRIES) {
    DPRINTLN(F("Cannot balance. Max. tries exceeded."));
    LegUtil::stopAllMotors(legs);
    setState(State::ErrorState);
  }

  if (balancingState == BalancingState::NotBalancing) {
    if (balancingAction.tries > 0) {
      // wait for leg/gyro to stabilize on subsequent tries
      delay(EXPAND_LEG_DLY);
    }

    determineBalancingState();
    if (balancingState != BalancingState::Balancing) {
      DPRINTLN(F("Cannot balance! Unknown gyro position."));
      setState(State::ErrorState);
      return;
    }

    expandLegs(balancingAction.legs[0], balancingAction.legs[1]);
  }

  switch (balancingState) {
    case BalancingState::Balancing:
      loopBalancingStep();
      break;
    case BalancingState::Error:
      DPRINTLN(F("Cannot balance."));
      setState(State::ErrorState);
      break;
    default:
      DPRINTLN(F("Invalid balancing state!"));
      setState(State::ErrorState);
  }
}

void Balancer::loopBalancingStep() {
  bool isAxeBalanced;
  float newPosition;

  if (balancingAction.axe == Axe::Pitch) {
    isAxeBalanced = gyro.isPitchBalanced();  
    newPosition = gyro.getPitch();
  } else {
    isAxeBalanced = gyro.isRollBalanced();
    newPosition = gyro.getRoll();
  }

  if (isAxeBalanced) {
    LegUtil::stopAllMotors(legs);
    balancingState = BalancingState::NotBalancing;
    return;
  }

  bool isBadDelta = this->isBadDelta(newPosition);
  if (!isBadDelta) {
    // Only save positions which are ok. Do not save bad deltas.
    balancingAction.previousPosition = newPosition;
    balancingAction.badDeltas = 0;
  } else {
    balancingAction.badDeltas++;
    if (balancingAction.badDeltas > MAX_BAD_DELTAS) {
      // something is wrong, rebalance
      Serial.print(F("Max. bad deltas exceeded!"));
      LegUtil::stopAllMotors(legs);
      balancingState = BalancingState::NotBalancing;
    }
  }
}

bool Balancer::isBadDelta(float newPosition) {
    // it's impossible no movement was made
    if (newPosition == balancingAction.previousPosition) {
      DPRINTLN(F("Delta is zero. Something might be wrong with gyro!"));
      return true;
    }

    bool directionOk;
    float delta = abs(newPosition - balancingAction.previousPosition);

    // from + to zero or from - to zero
    if (balancingAction.previousPosition < 0) {
      directionOk = newPosition > balancingAction.previousPosition;
    } else {
      directionOk = newPosition < balancingAction.previousPosition;
    }

    if (!directionOk) {
      DPRINTLN(F("Wrong direction!"));
      return true;
    }

    if (delta > MAX_DELTA_POSITION) {
      DPRINT(F("Delta not in range: "));
      DPRINT(delta);
      DPRINT(F(". Should be: "));
      DPRINTLN(MAX_DELTA_POSITION);
      return true;
    }

    return false;
}

void Balancer::determineBalancingState() {
  float pitch = gyro.getPitch();
  float roll = gyro.getRoll();

  balancingAction.badDeltas = 0;
  balancingAction.tries++;
  DPRINT(F("Balancing try no. "));
  DPRINT(balancingAction.tries);
  DPRINT(F("/"));
  DPRINT(MAX_TRIES);
  DPRINT(F(" ["));

  balancingState = BalancingState::Balancing;

  if (abs(pitch) > abs(roll)) {
    DPRINTLN(F("pitch]"));
    balancingAction.axe = Axe::Pitch;
    balancingAction.previousPosition = pitch;

    if (!GYRO_XY_FACE_UP) {
        if (pitch > 0.0 && !GYRO_XY_FACE_UP) {
          balancingAction.legs[0] = &legs[GYRO_LEG_TO_TRAILER_LEG(LEG_C)];
          balancingAction.legs[1] = &legs[GYRO_LEG_TO_TRAILER_LEG(LEG_D)];
        } else {
          balancingAction.legs[0] = &legs[GYRO_LEG_TO_TRAILER_LEG(LEG_A)];
          balancingAction.legs[1] = &legs[GYRO_LEG_TO_TRAILER_LEG(LEG_B)];
        }
    }

    // some complexities here because of different possible gyro rotations
    if (GYRO_XY_FACE_UP) {
        if (pitch > 0.0 && GYRO_XY_FACE_UP) {
          balancingAction.legs[0] = &legs[GYRO_LEG_TO_TRAILER_LEG(LEG_A)];
          balancingAction.legs[1] = &legs[GYRO_LEG_TO_TRAILER_LEG(LEG_C)];
        } else {
          balancingAction.legs[0] = &legs[GYRO_LEG_TO_TRAILER_LEG(LEG_D)];
          balancingAction.legs[1] = &legs[GYRO_LEG_TO_TRAILER_LEG(LEG_B)];
        }
    }
  } else {
    DPRINTLN(F("roll]"));
    balancingAction.axe = Axe::Roll;
    balancingAction.previousPosition = roll;

    if (!GYRO_XY_FACE_UP) {
        if (roll > 0.0) {
          balancingAction.legs[0] = &legs[GYRO_LEG_TO_TRAILER_LEG(LEG_D)];
          balancingAction.legs[1] = &legs[GYRO_LEG_TO_TRAILER_LEG(LEG_B)];
        } else {
          balancingAction.legs[0] = &legs[GYRO_LEG_TO_TRAILER_LEG(LEG_A)];
          balancingAction.legs[1] = &legs[GYRO_LEG_TO_TRAILER_LEG(LEG_C)];
        }
    }

    if (GYRO_XY_FACE_UP) {
        if (roll > 0.0) {
          balancingAction.legs[0] = &legs[GYRO_LEG_TO_TRAILER_LEG(LEG_A)];
          balancingAction.legs[1] = &legs[GYRO_LEG_TO_TRAILER_LEG(LEG_B)];
        } else {
          balancingAction.legs[0] = &legs[GYRO_LEG_TO_TRAILER_LEG(LEG_C)];
          balancingAction.legs[1] = &legs[GYRO_LEG_TO_TRAILER_LEG(LEG_D)];
        }
    }
  }

  DPRINT(F("Reference value: "));
  DPRINTLN(balancingAction.previousPosition, 4);
}

void Balancer::expandLegs(Leg *leg1, Leg *leg2) {
  LegUtil::stopAllMotors(legs);
  // we don't want to be restarting too fast
  delay(STOP_MOTORS_DLY);
  leg1->expand();
  leg2->expand();
  // expand legs at least one second before trying to expand another leg
  delay(EXPAND_LEG_DLY);
}

Leg *Balancer::getLegs() {
  return legs;
}

Gyro *Balancer::getGyro() {
  return &gyro;
}

State Balancer::getState() {
  return state;
}
