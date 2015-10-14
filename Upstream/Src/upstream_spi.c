/*
 * upstream_spi.c
 *
 *  Created on: 21/06/2015
 *      Author: Robert Fisk
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "upstream_interface_def.h"
#include "upstream_spi.h"
#include "upstream_statemachine.h"
#include "stm32f4xx_hal.h"
#include "usbd_def.h"
#include "board_config.h"
#include "interrupts.h"



SPI_HandleTypeDef			Hspi1;
UpstreamPacketTypeDef		UpstreamPacket0;
UpstreamPacketTypeDef		UpstreamPacket1;
UpstreamPacketTypeDef*		CurrentWorkingPacket;
UpstreamPacketTypeDef*		NextTxPacket		= NULL;			//Indicates we have a pending TX packet

InterfaceStateTypeDef				UpstreamInterfaceState		= UPSTREAM_INTERFACE_IDLE;
FreePacketCallbackTypeDef			PendingFreePacketCallback	= NULL;	//Indicates someone is waiting for a packet buffer to become available
SpiPacketReceivedCallbackTypeDef	ReceivePacketCallback		= NULL;	//Indicates someone is waiting for a received packet

uint32_t					TemporaryIncomingPacketLength;		//We don't actually care about what Downstream sends us when we are transmitting. We just need somewhere to put it so that our own packet length is not overwritten.
uint8_t						TxOkInterruptReceived = 0;
uint8_t						SpiInterruptCompleted = 0;
uint8_t						SentCommandClass;
uint8_t						SentCommand;


void Upstream_BeginTransmitPacketSize(void);
void Upstream_BeginTransmitPacketBody(void);
HAL_StatusTypeDef Upstream_CheckBeginPacketReception(void);
void Upstream_BeginReceivePacketSize(UpstreamPacketTypeDef* freePacket);
void Upstream_BeginReceivePacketBody(void);
void Upstream_SPIProcess(void);



void Upstream_InitSPI(void)
{
	UpstreamPacket0.Busy = NOT_BUSY;
	UpstreamPacket1.Busy = NOT_BUSY;

	Hspi1.Instance = SPI1;
	Hspi1.State = HAL_SPI_STATE_RESET;
	Hspi1.Init.Mode = SPI_MODE_MASTER;
	Hspi1.Init.Direction = SPI_DIRECTION_2LINES;
	Hspi1.Init.DataSize = SPI_DATASIZE_16BIT;
	Hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
	Hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
	Hspi1.Init.NSS = SPI_NSS_SOFT;
	Hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;	//42MHz APB2 / 32 = 1.3Mbaud
	Hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
	Hspi1.Init.TIMode = SPI_TIMODE_DISABLED;
	Hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_ENABLED;
	Hspi1.Init.CRCPolynomial = SPI_CRC_DEFAULTPOLYNOMIAL;
	HAL_SPI_Init(&Hspi1);

	if (DOWNSTREAM_TX_OK_ACTIVE)
	{
		TxOkInterruptReceived = 1;
	}
}



//Used by USB interface classes, and by our internal RX code.
HAL_StatusTypeDef Upstream_GetFreePacket(FreePacketCallbackTypeDef callback)
{
	if (UpstreamInterfaceState >= UPSTREAM_INTERFACE_ERROR)
	{
		return HAL_ERROR;
	}

	//Do we already have a queued callback?
	if (PendingFreePacketCallback != NULL)
	{
		UPSTREAM_SPI_FREAKOUT;
		return HAL_ERROR;
	}

	//Check if there is a free buffer now
	if (UpstreamPacket0.Busy == NOT_BUSY)
	{
		UpstreamPacket0.Busy = BUSY;
		callback(&UpstreamPacket0);
		return HAL_OK;
	}
	if (UpstreamPacket1.Busy == NOT_BUSY)
	{
		UpstreamPacket1.Busy = BUSY;
		callback(&UpstreamPacket1);
		return HAL_OK;
	}

	//Otherwise save requested address for when a buffer becomes free in the future
	PendingFreePacketCallback = callback;
	return HAL_OK;
}


UpstreamPacketTypeDef* Upstream_GetFreePacketImmediately(void)
{
	if (UpstreamInterfaceState >= UPSTREAM_INTERFACE_ERROR)
	{
		return NULL;
	}

	//We are expecting a free buffer now
	if (UpstreamPacket0.Busy == NOT_BUSY)
	{
		UpstreamPacket0.Busy = BUSY;
		return &UpstreamPacket0;
	}
	if (UpstreamPacket1.Busy == NOT_BUSY)
	{
		UpstreamPacket1.Busy = BUSY;
		return &UpstreamPacket1;
	}

	//Should not happen:
	UPSTREAM_SPI_FREAKOUT;
	return NULL;
}


//Used by USB interface classes, and by our internal RX code.
void Upstream_ReleasePacket(UpstreamPacketTypeDef* packetToRelease)
{
	FreePacketCallbackTypeDef tempCallback;
	
	if (UpstreamInterfaceState >= UPSTREAM_INTERFACE_ERROR)
	{
		return;
	}

	if ((packetToRelease != &UpstreamPacket0) &&
		(packetToRelease != &UpstreamPacket1))
	{
		UPSTREAM_SPI_FREAKOUT;
		return;
	}

	if (PendingFreePacketCallback != NULL)
	{
		tempCallback = PendingFreePacketCallback;	//In extreme situations, running this callback can trigger another request for a free packet,
		PendingFreePacketCallback = NULL;			//thereby causing GetFreePacket to freak out. So we need to clear the callback indicator first.
		tempCallback(packetToRelease);
	}
	else
	{
		packetToRelease->Busy = NOT_BUSY;
	}
}


//Used by USB interface classes only.
//OK to call when still transmitting another packet.
//Not OK to call when receiving or waiting for downstream reply,
//as we can't let the size/packet sequence get out of sync.
HAL_StatusTypeDef Upstream_TransmitPacket(UpstreamPacketTypeDef* packetToWrite)
{
	if (UpstreamInterfaceState >= UPSTREAM_INTERFACE_ERROR)
	{
		return HAL_ERROR;
	}
	
	//Sanity checks
	if ((packetToWrite != &UpstreamPacket0) &&
		(packetToWrite != &UpstreamPacket1))
	{
		UPSTREAM_SPI_FREAKOUT;
		return HAL_ERROR;
	}
	if ((packetToWrite->Busy != BUSY) ||
		(packetToWrite->Length16 < UPSTREAM_PACKET_LEN_MIN_16) ||
		(packetToWrite->Length16 > UPSTREAM_PACKET_LEN_16))
	{
		UPSTREAM_SPI_FREAKOUT;
		return HAL_ERROR;
	}
	if (NextTxPacket != NULL)
	{
		UPSTREAM_SPI_FREAKOUT;
		return HAL_ERROR;
	}

	switch (UpstreamInterfaceState)
	{
	case UPSTREAM_INTERFACE_TX_SIZE_WAIT:
	case UPSTREAM_INTERFACE_TX_SIZE:
	case UPSTREAM_INTERFACE_TX_PACKET_WAIT:
	case UPSTREAM_INTERFACE_TX_PACKET:
		NextTxPacket = packetToWrite;
		break;

	case UPSTREAM_INTERFACE_IDLE:
		UpstreamInterfaceState = UPSTREAM_INTERFACE_TX_SIZE_WAIT;
		CurrentWorkingPacket = packetToWrite;
		SentCommandClass = CurrentWorkingPacket->CommandClass;
		SentCommand = CurrentWorkingPacket->Command;

		//Downstream may have set TxOk pin before we wanted to transmit.
		//In this case we can go ahead and transmit now.
		if (TxOkInterruptReceived)
		{
			TxOkInterruptReceived = 0;
			Upstream_BeginTransmitPacketSize();
		}
		break;

	default:
		UPSTREAM_SPI_FREAKOUT;
		return HAL_ERROR;
	}
	return HAL_OK;
}



//Called at the end of the SPI TxRx transfer,
//at SPI1 interrupt priority. Assume *hspi points to our hspi1.
//We TxRx our outgoing packet because the SPI hardware freaks out if we only Tx it :-/
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
	SpiInterruptCompleted = 1;

	//Elevate priority here to stop EXT3I sneaking in
	//before we have a chance to process UpstreamInterfaceState change.
	__set_BASEPRI(INT_PRIORITY_USB << (8 - __NVIC_PRIO_BITS));
}



//Preemption protection wrapper around Upstream_SPIProcess()
//We must protect against preemption by USB and EXT3 interrupts at priority 10!
void Upstream_SPIProcess_InterruptSafe(void)
{
	//This is done on SPI interrupt callback...
	//__set_BASEPRI(INT_PRIORITY_OTG_FS << (8 - __NVIC_PRIO_BITS));

	if (SpiInterruptCompleted == 0)
	{
		return;
	}
	SpiInterruptCompleted = 0;
	Upstream_SPIProcess();
	__set_BASEPRI(0);
}



//Called from main().
//Must be protected against preemption by USB and EXT3 interrupts at priority 10!
void Upstream_SPIProcess(void)
{
	SpiPacketReceivedCallbackTypeDef tempPacketCallback;
	UpstreamPacketTypeDef* tempPacketToFree;

	SPI1_NSS_DEASSERT;

	if (UpstreamInterfaceState >= UPSTREAM_INTERFACE_ERROR)
	{
		return;
	}

	//Finished transmitting packet size
	if (UpstreamInterfaceState == UPSTREAM_INTERFACE_TX_SIZE)
	{
		UpstreamInterfaceState = UPSTREAM_INTERFACE_TX_PACKET_WAIT;
		if (TxOkInterruptReceived)
		{
			TxOkInterruptReceived = 0;
			Upstream_BeginTransmitPacketBody();
		}
		return;
	}

	//Finished transmitting packet body
	if (UpstreamInterfaceState == UPSTREAM_INTERFACE_TX_PACKET)
	{
		if ((PendingFreePacketCallback != NULL) && (NextTxPacket == NULL))
		{
			UPSTREAM_SPI_FREAKOUT;
			return;
		}

		tempPacketToFree = CurrentWorkingPacket;

		if (NextTxPacket != NULL)
		{
			//NextTxPacket has already passed the checks in Upstream_TransmitPacket.
			//So we just need to pass it to HAL_SPI_Transmit_DMA.
			UpstreamInterfaceState = UPSTREAM_INTERFACE_TX_SIZE_WAIT;
			CurrentWorkingPacket = NextTxPacket;
			NextTxPacket = NULL;
			SentCommandClass = CurrentWorkingPacket->CommandClass;
			SentCommand = CurrentWorkingPacket->Command;
			if (TxOkInterruptReceived)
			{
				TxOkInterruptReceived = 0;
				Upstream_BeginTransmitPacketSize();
			}
		}
		else
		{
			//No packet queued for transmission:
			UpstreamInterfaceState = UPSTREAM_INTERFACE_IDLE;
			if (ReceivePacketCallback != NULL)
			{
				Upstream_CheckBeginPacketReception();
			}
		}

		//Release old packet after moving Next to Current
		Upstream_ReleasePacket(tempPacketToFree);
		return;
	}
	


	if (UpstreamInterfaceState == UPSTREAM_INTERFACE_RX_SIZE)
	{
		if ((CurrentWorkingPacket->Length16 < UPSTREAM_PACKET_LEN_MIN_16) ||
			(CurrentWorkingPacket->Length16 > UPSTREAM_PACKET_LEN_16))
		{
			UPSTREAM_SPI_FREAKOUT;
			return;
		}
		UpstreamInterfaceState = UPSTREAM_INTERFACE_RX_PACKET_WAIT;
		if (TxOkInterruptReceived)
		{
			TxOkInterruptReceived = 0;
			Upstream_BeginReceivePacketBody();
		}
		return;
	}

	if (UpstreamInterfaceState == UPSTREAM_INTERFACE_RX_PACKET)
	{
		UpstreamInterfaceState = UPSTREAM_INTERFACE_IDLE;
		if (ReceivePacketCallback == NULL)
		{
			UPSTREAM_SPI_FREAKOUT;
			return;
		}

		if ((CurrentWorkingPacket->CommandClass == COMMAND_CLASS_ERROR) &&
			(CurrentWorkingPacket->Command == COMMAND_ERROR_DEVICE_DISCONNECTED))
		{
			Upstream_ReleasePacket(CurrentWorkingPacket);
			ReceivePacketCallback = NULL;
			Upstream_StateMachine_DeviceDisconnected();
			return;
		}

		if (((CurrentWorkingPacket->CommandClass & COMMAND_CLASS_MASK) != (SentCommandClass & COMMAND_CLASS_MASK)) ||
			(CurrentWorkingPacket->Command != SentCommand))
		{
			UPSTREAM_SPI_FREAKOUT;
			Upstream_ReleasePacket(CurrentWorkingPacket);
			CurrentWorkingPacket = NULL;		//Call back with a NULL packet to indicate error
		}

		//USB interface may want to receive another packet immediately,
		//so clear ReceivePacketCallback before the call.
		//It is the callback's responsibility to release the packet buffer we are passing to it!
		tempPacketCallback = ReceivePacketCallback;
		ReceivePacketCallback = NULL;
		tempPacketCallback(CurrentWorkingPacket);
		return;
	}


	//case default:
	UPSTREAM_SPI_FREAKOUT;
}


//Used by USB interface classes.
//Ok to call when idle or transmitting.
//Not OK to call when receiving or waiting for downstream reply.
HAL_StatusTypeDef Upstream_ReceivePacket(SpiPacketReceivedCallbackTypeDef callback)
{
	if (UpstreamInterfaceState >= UPSTREAM_INTERFACE_ERROR)
	{
		return HAL_ERROR;
	}
	
	if (ReceivePacketCallback != NULL)
	{
		UPSTREAM_SPI_FREAKOUT;
		return HAL_ERROR;
	}

	ReceivePacketCallback = callback;
	return Upstream_CheckBeginPacketReception();
}


//Internal use only.
HAL_StatusTypeDef Upstream_CheckBeginPacketReception(void)
{
	if (UpstreamInterfaceState >= UPSTREAM_INTERFACE_ERROR)
	{
		return HAL_ERROR;
	}
	
	if (UpstreamInterfaceState >= UPSTREAM_INTERFACE_RX_SIZE_WAIT)
	{
		UPSTREAM_SPI_FREAKOUT;
		return HAL_ERROR;
	}

	if (UpstreamInterfaceState == UPSTREAM_INTERFACE_IDLE)
	{
		UpstreamInterfaceState = UPSTREAM_INTERFACE_RX_SIZE_WAIT;
		if (TxOkInterruptReceived)
		{
			TxOkInterruptReceived = 0;
			Upstream_GetFreePacket(Upstream_BeginReceivePacketSize);
		}
	}
	return HAL_OK;
}


//This is called by EXTI3 falling edge interrupt,
//indicating that downstream is ready for the next transaction.
void Upstream_TxOkInterrupt(void)
{
	if (UpstreamInterfaceState >= UPSTREAM_INTERFACE_ERROR)
	{
		return;
	}
	
	switch (UpstreamInterfaceState)
	{
	case UPSTREAM_INTERFACE_IDLE:
		TxOkInterruptReceived = 1;
		break;

	case UPSTREAM_INTERFACE_TX_SIZE_WAIT:
		Upstream_BeginTransmitPacketSize();
		break;

	case UPSTREAM_INTERFACE_TX_PACKET_WAIT:
		Upstream_BeginTransmitPacketBody();
		break;

	case UPSTREAM_INTERFACE_RX_SIZE_WAIT:
		Upstream_GetFreePacket(Upstream_BeginReceivePacketSize);
		break;

	case UPSTREAM_INTERFACE_RX_PACKET_WAIT:
		Upstream_BeginReceivePacketBody();
		break;

	default:
		UPSTREAM_SPI_FREAKOUT;
	}
}


void Upstream_BeginTransmitPacketSize(void)
{
	UpstreamInterfaceState = UPSTREAM_INTERFACE_TX_SIZE;
	SPI1_NSS_ASSERT;
	if (HAL_SPI_TransmitReceive_IT(&Hspi1,
								   (uint8_t*)&CurrentWorkingPacket->Length16,
								   (uint8_t*)&TemporaryIncomingPacketLength,
								   2) != HAL_OK)		//We only need to write one word, but the peripheral library freaks out...
	{
		UPSTREAM_SPI_FREAKOUT;
	}
}


void Upstream_BeginTransmitPacketBody(void)
{
	UpstreamInterfaceState = UPSTREAM_INTERFACE_TX_PACKET;
	SPI1_NSS_ASSERT;

	if (HAL_SPI_TransmitReceive_IT(&Hspi1,
								   &CurrentWorkingPacket->CommandClass,
								   &CurrentWorkingPacket->CommandClass,
								   ((CurrentWorkingPacket->Length16 < 2) ? 2 : CurrentWorkingPacket->Length16)) != HAL_OK)
	{
		UPSTREAM_SPI_FREAKOUT;
	}
}


//Internal use only.
//Called when we want to receive downstream packet, and a packet buffer has become free.
void Upstream_BeginReceivePacketSize(UpstreamPacketTypeDef* freePacket)
{
	if (UpstreamInterfaceState >= UPSTREAM_INTERFACE_ERROR)
	{
		return;
	}
	
	if (UpstreamInterfaceState != UPSTREAM_INTERFACE_RX_SIZE_WAIT)
	{
		UPSTREAM_SPI_FREAKOUT;
		return;
	}
	UpstreamInterfaceState = UPSTREAM_INTERFACE_RX_SIZE;
	CurrentWorkingPacket = freePacket;
	CurrentWorkingPacket->Length16 = 0;		//Our RX buffer is used by HAL_SPI_Receive_DMA as dummy TX data, we set Length to 0 so downstream will know this is a dummy packet.
	SPI1_NSS_ASSERT;
	TemporaryIncomingPacketLength = 0;
	if (HAL_SPI_TransmitReceive_IT(&Hspi1,
								   (uint8_t*)&TemporaryIncomingPacketLength,
								   (uint8_t*)&CurrentWorkingPacket->Length16,
								   2) != HAL_OK)		//We only need to write one word, but the peripheral library freaks out...
	{
		UPSTREAM_SPI_FREAKOUT;
	}
}


void Upstream_BeginReceivePacketBody(void)
{
	UpstreamInterfaceState = UPSTREAM_INTERFACE_RX_PACKET;
	SPI1_NSS_ASSERT;
	if (HAL_SPI_TransmitReceive_IT(&Hspi1,
								   &CurrentWorkingPacket->CommandClass,
								   &CurrentWorkingPacket->CommandClass,
								   ((CurrentWorkingPacket->Length16 < 2) ? 2 : CurrentWorkingPacket->Length16)) != HAL_OK)
	{
		UPSTREAM_SPI_FREAKOUT;
	}
}



//Something bad happened! Possibly CRC error...
void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
	SpiPacketReceivedCallbackTypeDef tempPacketCallback;

	if (UpstreamInterfaceState >= UPSTREAM_INTERFACE_ERROR)
	{
		return;
	}
	
	UPSTREAM_SPI_FREAKOUT;

	if (ReceivePacketCallback != NULL)
	{
		tempPacketCallback = ReceivePacketCallback;
		ReceivePacketCallback = NULL;
		tempPacketCallback(NULL);			//Call back with a NULL packet to indicate error
	}
}


