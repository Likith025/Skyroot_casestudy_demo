/*
 * flight_sequencer_app.h
 *
 *  Created on: 12-Oct-2026
 *      Author: likith
 */
#ifndef FLIGHT_SEQUENCER_APP_H
#define FLIGHT_SEQUENCER_APP_H

#include <stdbool.h>

/*
 * Call after osKernelInitialize() and after UART3 has been initialized.
 * This demo uses simulated sensor payloads; it does not implement RS422
 * electrical interfaces or real sensor UART reception.
 */
void FlightSequencer_Start(void);

#endif
