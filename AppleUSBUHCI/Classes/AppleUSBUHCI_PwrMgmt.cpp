/*
 * Copyright (c) 2004-2007 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.2 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.  
 * Please see the License for the specific language governing rights and 
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */


#include <IOKit/usb/USB.h>
#include <IOKit/usb/IOUSBLog.h>
#include <IOKit/usb/IOUSBRootHubDevice.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOHibernatePrivate.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <libkern/libkern.h>

#ifndef kACPIDevicePathKey
#define kACPIDevicePathKey			"acpi-path"
#endif

#include "AppleUSBUHCI.h"

#define super IOUSBControllerV3

// ========================================================================
#pragma mark Public power management interface
// ========================================================================

#define _controllerCanSleep				_expansionData->_controllerCanSleep

void
AppleUSBUHCI::CheckSleepCapability(void)
{
    if (_device->getProperty("built-in") && (_errataBits & kErrataICH6PowerSequencing)) 
	{
		// The ICH6 UHCI drivers on a Transition system just magically work on sleep/wake
		// so we will just hard code those. Other systems will have to be evaluated later
        setProperty("Card Type","Built-in");
        _controllerCanSleep = true;
    }
    else 
	{
        // This appears to be necessary
		setProperty("Card Type","PCI");
		_controllerCanSleep = false;
    }
	// if we have an ExpressCard attached (non-zero port), then we need to register for some special messages to allow us to override the Resume Enables 
	// for that port (some cards disconnect when the ExpressCard power goes away and we would like to ignore these extra detach events.
	if ((_ExpressCardPort = ExpressCardPort(_device)))
	{
		_device->callPlatformFunction(
									   /* function */ "RegisterDebugDriver",
									   /* waitForFunction */ false,
									   /* provider nub� � */ _device,
									   /* unused � */ (void *) this,
									   /* unused � */ (void *) NULL,
									   /* unused � */ (void *) NULL );
	}
	_badExpressCardAttached = false;
	
	// Call registerService() so that the IOUSBController object is published and clients (like Prober) can find it
	registerService();
}



IOReturn
AppleUSBUHCI::callPlatformFunction(const OSSymbol *functionName,
                                   bool waitForFunction,
                                   void *param1, void *param2,
                                   void *param3, void *param4)
{
    USBLog(3, "%s[%p]::callPlatformFunction(%s)", getName(), this, functionName->getCStringNoCopy());
    

	if (!strncmp(functionName->getCStringNoCopy(), "SetDebugDriverPowerState", 24))
	{
		if (param1)
		{
			// system woke from sleep -- do nothing
		}
		else
		{
			// system is going to sleep
			//
			// check if we are the controller for an expressCard port.  If so, then we need to ignore disconnects on suspend for that port  
			// this will avoid the problem where the ExpressCard power goes away, and it looks like the device detaching -- thus waking the machine!
			if ((_badExpressCardAttached) && (_ExpressCardPort > 0) && (_errataBits & kErrataSupportsPortResumeEnable))
			{
				// set PCI_RES register to enable ports to wake the computer.
				_device->configWrite8(kUHCI_PCI_RES, 0x03 & ~(1 << (_ExpressCardPort-1)));	// clear the bit for the ExpressCardPort		
			}
		}
	}

    return super::callPlatformFunction(functionName, waitForFunction, param1, param2, param3, param4);
}



// ========================================================================
#pragma mark Internal methods
// ========================================================================



void			
AppleUSBUHCI::ResumeController(void)
{
    UInt16			cmd;
	int				i;
    
	showRegisters(7, "+ResumeController");
	
	cmd = ioRead16(kUHCI_CMD);
	if (cmd & kUHCI_CMD_RS)
	{
		USBLog(3, "AppleUSBUHCI[%p]::ResumeController - already running - returning", this);
		return;
	}
	
	// I need to save the existing frame list before I turn on processing so I can send SOF only for 10ms after we turn the controller on
	for (i=0;i < kUHCI_NVFRAMES; i++)
	{
		_frameList[i] |= HostToUSBLong(kUHCI_FRAME_T);
	}
			
	if (cmd & kUHCI_CMD_EGSM)
	{
		USBLog(5, "AppleUSBUHCI[%p]::ResumeController controller is globally suspended - forcing resume", this);
		cmd |= kUHCI_CMD_FGR;
		ioWrite16(kUHCI_CMD, cmd);
		cmd = ioRead16(kUHCI_CMD);
		USBLog(5, "AppleUSBUHCI[%p]::ResumeController after EGSM->FGR, cmd is[%p]", this, (void*)cmd);
	}
    
	if (cmd & kUHCI_CMD_FGR)
	{
		// this could either be because the remote wwakeup caused this state or because we did above
		// need to wait 20ms
		IOSleep(20);
		cmd &= ~kUHCI_CMD_FGR;
		cmd &= ~kUHCI_CMD_EGSM;
		ioWrite16(kUHCI_CMD, cmd);
	}
	if ((cmd & (kUHCI_CMD_MAXP | kUHCI_CMD_CF)) != (kUHCI_CMD_MAXP | kUHCI_CMD_CF))
	{
		USBLog(5, "AppleUSBUHCI[%p]::ResumeController marking MAXP and CF", this);
		cmd |= (kUHCI_CMD_MAXP | kUHCI_CMD_CF);
		ioWrite16(kUHCI_CMD, cmd);
	}
	
	// restore the frame list register
    if (_framesPaddr != NULL) 
	{
		USBLog(5, "AppleUSBUHCI[%p]::ResumeController setting FRBASEADDR[%p]", this, (void*)_framesPaddr);
        ioWrite32(kUHCI_FRBASEADDR, _framesPaddr);
	}
	
	USBLog(5, "AppleUSBUHCI[%p]::ResumeController starting controller", this);
	Run(true);
	
	// wait 10 ms for the device to recover
	IOSleep(10);
	
	// restore the list
	for (i=0;i < kUHCI_NVFRAMES; i++)
	{
		_frameList[i] &= ~HostToUSBLong(kUHCI_FRAME_T);
	}
    
	USBLog(7, "AppleUSBUHCI[%p]::ResumeController resume done, cmd %x, status %x ports[%p, %p]", this, ioRead16(kUHCI_CMD), ioRead16(kUHCI_STS),(void*)ReadPortStatus(0), (void*)ReadPortStatus(1));
	showRegisters(7, "-ResumeController");
}



void			
AppleUSBUHCI::SuspendController(void)
{
    UInt16				cmd, value;
	int					i;
    
    USBLog(5, "%s[%p]::SuspendController", getName(), this);
    USBLog(5, "%s[%p]: cmd state %x, status %x", getName(), this, ioRead16(kUHCI_CMD), ioRead16(kUHCI_STS));

    // Stop the controller
    Run(false);
    
	for (i=0; i< 2; i++)
	{
		value = ReadPortStatus(i) & kUHCI_PORTSC_MASK;
		if (value & kUHCI_PORTSC_PED)
		{
			if (value & kUHCI_PORTSC_SUSPEND)
			{
				USBLog(5, "AppleUSBUHCI[%p]::SuspendController - port[%d] is suspended [%p]", this, i, (void*)value);
			}
			else
			{
				USBLog(5, "AppleUSBUHCI[%p]::SuspendController - port[%d] is enabled but not suspended [%p]", this, i, (void*)value);
			}
		}
		else
		{
			USBLog(5, "AppleUSBUHCI[%p]::SuspendController - port[%d] is not enabled [%p]", this, i, (void*)value);
		}
		
		// only do this for controllers with overcurrent additions.
		if ((_errataBits & kErrataUHCISupportsOvercurrent) && (value & kUHCI_PORTSC_OCI))  // Is the latched Overcurrent set?
		{
			// if so, clear it or we won't suspend.
			USBLog(1, "AppleUSBUHCI[%p]::SuspendController - port[%d] had the overcurrent bit set.  Clearing it", this, i);
			WritePortStatus(i, kUHCI_PORTSC_OCI); // clear overcurrent indicator
		}
	}
    // Put the controller in Global Suspend
    cmd = ioRead16(kUHCI_CMD) & ~kUHCI_CMD_FGR;
    cmd |= kUHCI_CMD_EGSM;
    ioWrite16(kUHCI_CMD, cmd);
	_myBusState = kUSBBusStateSuspended;   
    IOSleep(3);
    USBLog(5, "%s[%p]: suspend done, cmd %x, status %x", getName(), this, ioRead16(kUHCI_CMD), ioRead16(kUHCI_STS));
}



IOReturn				
AppleUSBUHCI::SaveControllerStateForSleep(void)
{	
	
    USBLog(5, "AppleUSBUHCI[%p]::SaveControllerStateForSleep cancelling rhTimer", this);
	USBLog(5, "AppleUSBUHCI[%p]::SaveControllerStateForSleep SUSPEND - disabling interrupt", this);
	// put the controller into suspend (which suspends all of the downstream ports)
	SuspendController();
	
	return kIOReturnSuccess;
}



IOReturn				
AppleUSBUHCI::RestoreControllerStateFromSleep(void)
{

	USBLog(5, "AppleUSBUHCI[%p]::RestoreControllerStateFromSleep RUN - resuming controller", this);
	ResumeController();

	return kIOReturnSuccess;
}



//
// ResetControllerState
// puts the controller into a known state - data structures in place, but interrupts disabled the controller halted
//
IOReturn
AppleUSBUHCI::ResetControllerState(void)
{
	UInt32				value;
	int					i;

	USBLog(5, "AppleUSBUHCI[%p]::+ResetControllerState", this);

	// reset the controller
    Command(kUHCI_CMD_HCRESET);
    for(i=0; (i < kUHCI_RESET_DELAY) && (ioRead16(kUHCI_CMD) & kUHCI_CMD_HCRESET); i++) 
	{
        IOSleep(1);
    }
    if (i >= kUHCI_RESET_DELAY) 
	{
        USBError(1, "AppleUSBUHCI[%p]::ResetControllerStatecontroller - reset failed", this);
        return kIOReturnTimeout;
    }
    USBLog(5, "AppleUSBUHCI[%p]::ResetControllerStatecontroller - reset done after %d spins", this, i);

	// restore the frame list register
    if (_framesPaddr != NULL) 
	{
        ioWrite32(kUHCI_FRBASEADDR, _framesPaddr);
	}
	
	// Use 64-byte packets, and mark controller as configured
	Command(kUHCI_CMD_MAXP | kUHCI_CMD_CF);

    USBLog(5, "AppleUSBUHCI[%p]::-ResetControllerState", this);
	return kIOReturnSuccess;
}



IOReturn
AppleUSBUHCI::RestartControllerFromReset(void)
{
	USBLog(5, "AppleUSBUHCI[%p]::RestartControllerFromReset - _myBusState(%d) CMD(%p) STS(%p) FRBASEADDR(%p) IOPCIConfigCommand(%p)", this, (int)_myBusState, (void*)ioRead16(kUHCI_CMD), (void*)ioRead16(kUHCI_STS), (void*)ioRead32(kUHCI_FRBASEADDR), (void*)_device->configRead16(kIOPCIConfigCommand));

	Run(true);

	// prepare the _saveInterrupts variable for later enabling
	_saveInterrupts = kUHCI_INTR_TIE | kUHCI_INTR_RIE | kUHCI_INTR_IOCE | kUHCI_INTR_SPIE;
	USBLog(5, "AppleUSBUHCI[%p]::RestartControllerFromReset - I set _saveInterrupts to (%p)", this, (void*)_saveInterrupts);
			
	return kIOReturnSuccess;
}



IOReturn
AppleUSBUHCI::EnableInterruptsFromController(bool enable)
{
	if (enable)
	{
		USBLog(5, "AppleUSBUHCI[%p]::EnableInterruptsFromController - enabling interrupts, USBIntr(%p) _savedUSBIntr(%p)", this, (void*)ioRead16(kUHCI_INTR), (void*)_saveInterrupts);
		ioWrite16(kUHCI_INTR, _saveInterrupts);
		_saveInterrupts = 0;
		EnableUSBInterrupt(true);
	}
	else
	{
		_saveInterrupts = ioRead16(kUHCI_INTR);
		ioWrite16(kUHCI_INTR, 0);
		EnableUSBInterrupt(false);
		USBLog(5, "AppleUSBUHCI[%p]::EnableInterruptsFromController - interrupts disabled, _saveInterrupts(%p)", this, (void*)_saveInterrupts);
	}
	
	return kIOReturnSuccess;
}



IOReturn
AppleUSBUHCI::DozeController(void)
{
	showRegisters(7, "+DozeController -  stopping controller");
	Run(false);

	_myBusState = kUSBBusStateSuspended;
	return kIOReturnSuccess;
}



IOReturn				
AppleUSBUHCI::WakeControllerFromDoze(void)
{
	Run(true);
	_myBusState = kUSBBusStateRunning;
	showRegisters(7, "-WakeControllerFromDoze");
	return kIOReturnSuccess;
}



IOReturn
AppleUSBUHCI::powerStateWillChangeTo ( IOPMPowerFlags capabilities, unsigned long newState, IOService* whichDevice)
{
	USBLog(5, "AppleUSBUHCI[%p]::powerStateWillChangeTo new state (%d)", this, (int)newState);
	showRegisters(7, "powerStateWillChangeTo");
	return super::powerStateWillChangeTo(capabilities, newState, whichDevice);
}



IOReturn
AppleUSBUHCI::powerStateDidChangeTo ( IOPMPowerFlags capabilities, unsigned long newState, IOService* whichDevice)
{
	USBLog(5, "AppleUSBUHCI[%p]::powerStateDidChangeTo new state (%d)", this, (int)newState);
	showRegisters(7, "powerStateDidChangeTo");
	return super::powerStateDidChangeTo(capabilities, newState, whichDevice);
}



void
AppleUSBUHCI::powerChangeDone ( unsigned long fromState)
{
	unsigned long newState = getPowerState();
	
	USBLog((fromState == newState) ? 7 : 5, "AppleUSBUHCI[%p]::powerChangeDone from state (%d) to state (%d) _controllerAvailable(%s)", this, (int)fromState, (int)newState, _controllerAvailable ? "true" : "false");
	if (_controllerAvailable)
		showRegisters(7, "powerChangeDone");
	super::powerChangeDone(fromState);
}



#pragma mark ���� Utility functions ����
static IOACPIPlatformDevice * CopyACPIDevice( IORegistryEntry * device )
{
	IOACPIPlatformDevice *  acpiDevice = 0;
	OSString *				acpiPath;

	if (device)
	{
		acpiPath = (OSString *) device->copyProperty(kACPIDevicePathKey);
		if (acpiPath && !OSDynamicCast(OSString, acpiPath))
		{
			acpiPath->release();
			acpiPath = 0;
		}

		if (acpiPath)
		{
			IORegistryEntry * entry;

			entry = IORegistryEntry::fromPath(acpiPath->getCStringNoCopy());
			acpiPath->release();

			if (entry && entry->metaCast("IOACPIPlatformDevice"))
				acpiDevice = (IOACPIPlatformDevice *) entry;
			else if (entry)
				entry->release();
		}
	}

	return (acpiDevice);
}

static bool HasExpressCardUSB( IORegistryEntry * acpiDevice, UInt32 * portnum )
{
	const IORegistryPlane *	acpiPlane;
	bool					match = false;
	IORegistryIterator *	iter;
	IORegistryEntry *		entry;

	do {
		acpiPlane = acpiDevice->getPlane( "IOACPIPlane" );
		if (!acpiPlane)
			break;

		// acpiDevice is the USB controller in ACPI plane.
		// Recursively iterate over children of acpiDevice.

		iter = IORegistryIterator::iterateOver(
				/* start */	acpiDevice,
				/* plane */	acpiPlane,
				/* options */ kIORegistryIterateRecursively);

		if (iter)
		{
			while (!match && (entry = iter->getNextObject()))
			{
				// USB port must be a leaf node (no child), and
				// must be an IOACPIPlatformDevice.

				if ((entry->getChildEntry(acpiPlane) == 0) &&
					entry->metaCast("IOACPIPlatformDevice"))
				{
					IOACPIPlatformDevice * port;
					port = (IOACPIPlatformDevice *) entry;

					// Express card port? Is port ejectable?

					if (port->validateObject( "_EJD" ) == kIOReturnSuccess)
					{
						// Determining the USB port number.
						if (portnum)
							*portnum = strtoul(port->getLocation(), NULL, 10);
						match = true;
					}
				}
			}

			iter->release();
		}
	}
	while (false);
	
	return match;
}

// Checks for ExpressCard connected to this controller, and returns the port number (1 based)
// Will return 0 if no ExpressCard is connected to this controller.
//
UInt32 AppleUSBUHCI::ExpressCardPort( IOService * provider )
{
	IOACPIPlatformDevice *	acpiDevice;
	UInt32					portNum = 0;
	bool					isPCIeUSB;
	
	acpiDevice = CopyACPIDevice( provider );
	if (acpiDevice)
	{
		isPCIeUSB = HasExpressCardUSB( acpiDevice, &portNum );	
		acpiDevice->release();
	}
	return(portNum);
}