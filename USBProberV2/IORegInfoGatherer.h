/*
 * Copyright � 1998-2012 Apple Inc.  All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */


#include <AvailabilityMacros.h>


#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <IOKit/IOMessage.h>
#import <IOKit/IOCFPlugIn.h>
#import <IOKit/usb/IOUSBLib.h>
#import <mach/mach_port.h>
#import "IORegOutlineViewNode.h"


enum {
    kIOUSB_Plane = 0,
    kIOService_Plane = 1
};

@class IORegInfoGatherer;

@protocol IORegGathererListener <NSObject>

- (void)ioRegInfoGathererInformationDidChange:(IORegInfoGatherer *)aGatherer;

@end

@interface IORegInfoGatherer : NSObject {
    id      _listener;
    int     _plane;
    
    OutlineViewNode *       _rootNode;
    CFRunLoopSourceRef      _runLoopSource;
}

- initWithListener:(id <IORegGathererListener> )listener rootNode:(OutlineViewNode *)rootNode plane:(int)plane;

- (BOOL)registerForUSBNotifications;
- (void)unregisterForUSBNotifications;

- (void)refreshData:(BOOL)shouldForce;

- (void)setPlane:(int)plane;

@end

