/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * RTW88Kext.hpp — top-level IOService provider matching
 */
#pragma once

#include <IOKit/IOService.h>

class RTW88Kext : public IOService {
    OSDeclareDefaultStructors(RTW88Kext)

public:
    bool init(OSDictionary *props) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

    IOService *probe(IOService *provider, SInt32 *score) override;
};
