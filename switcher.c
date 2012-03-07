#include <IOKit/IOKitLib.h>
#include <stdio.h>

#define BOOL int

typedef enum {
    modeForceIntegrated,
    modeForceDiscrete,
    modeDynamicSwitching,
    modeToggleGPU
} SwitcherMode;

#define kDriverClassName "AppleGraphicsControl"

static io_connect_t switcherConnect = IO_OBJECT_NULL;

// Stuff to look at:
// nvram -p -> gpu_policy
// bootargs: agc=0/1: flags in dmesg
// Compile: gcc -o switcher switcher.c -framework IOKit
// boot arguments: 
// sudo nvram boot-args="agc=??? agclog=??? agcdebug=???"

// User client method dispatch selectors.
enum {
    kOpen,
    kClose,
    kSetMuxState,
    kGetMuxState,
    kSetExclusive,
    kDumpState,
    kUploadEDID,
    kGetAGCData,
    kGetAGCData_log1,
    kGetAGCData_log2,
    kNumberOfMethods
};

typedef enum {
    muxDisableFeature    = 0, // set only
    muxEnableFeature    = 1, // set only
    
    muxFeatureInfo        = 0, // get: returns a uint64_t with bits set according to FeatureInfos, 1=enabled
    muxFeatureInfo2        = 1, // get: same as MuxFeatureInfo
    
    muxForceSwitch        = 2, // set: force Graphics Switch regardless of switching mode
    // get: always returns 0xdeadbeef
    
    muxPowerGPU            = 3, // set: power down a gpu, pretty useless since you can't power down the igp and the dedicated gpu is powered down automatically
    // get: maybe returns powered on graphics cards, 0x8 = integrated, 0x88 = discrete (or probably both, since integrated never gets powered down?)
    
    muxGpuSelect        = 4, // set/get: Dynamic Switching on/off with [2] = 0/1 (the same as if you click the checkbox in systemsettings.app)
    
    // TODO: Test what happens on older mbps when switchpolicy = 0
    // Changes if you're able to switch in systemsettings.app without logout
    muxSwitchPolicy        = 5, // set: 0 = dynamic switching, 2 = no dynamic switching, exactly like older mbp switching, 3 = no dynamic stuck, others unsupported
    // get: possibly inverted?
    
    muxUnknown            = 6, // get: always 0xdeadbeef
    
    muxGraphicsCard        = 7, // get: returns active graphics card
    muxDebug            = 8, // get: sometimes 0xffffffff, TODO: figure out what that means
    
} muxState;

typedef enum {
    Policy,
    Auto_PowerDown_GPU,
    Dynamic_Switching,
    GPU_Powerpolling, // Inverted: Disable Feature enables it and vice versa
    Defer_Policy,
    Synchronous_Launch,
    Backlight_Control=8,
    Recovery_Timeouts,
    Power_Switch_Debounce,
    Logging=16,
    Display_Capture_Switch,
    No_GL_HDA_busy_idle_registration,
    muxFeaturesCount
} muxFeature;

static BOOL getMuxState(io_connect_t connect, uint64_t input, uint64_t *output) {
    kern_return_t kernResult;
    uint32_t outputCount = 1;
    uint64_t scalarI_64[2] = { 1 /* Always 1 (kMuxControl?) */, input /* Feature Info */ };
    
    kernResult = IOConnectCallScalarMethod(connect,       // an io_connect_t returned from IOServiceOpen().
                                           kGetMuxState,  // selector of the function to be called via the user client.
                                           scalarI_64,    // array of scalar (64-bit) input values.
                                           2,             // the number of scalar input values.
                                           output,        // array of scalar (64-bit) output values.
                                           &outputCount); // pointer to the number of scalar output values.
    if (kernResult == KERN_SUCCESS) {
        printf("getMuxState was successful (count=%d, value=0x%08llx).\n", outputCount, *output);
    } else {
        printf("getMuxState returned 0x%08x.\n", kernResult);
    }
    return kernResult == KERN_SUCCESS;
}

static BOOL setMuxState(io_connect_t connect, muxState state, uint64_t arg) {
    kern_return_t kernResult;
    uint64_t scalarI_64[3] = { 1 /* always? */, (uint64_t) state, arg };
    
    kernResult = IOConnectCallScalarMethod(connect,      // an io_connect_t returned from IOServiceOpen().
                                           kSetMuxState, // selector of the function to be called via the user client.
                                           scalarI_64,   // array of scalar (64-bit) input values.
                                           3,            // the number of scalar input values.
                                           NULL,         // array of scalar (64-bit) output values.
                                           0);           // pointer to the number of scalar output values.
    if (kernResult == KERN_SUCCESS) {
        printf("setMuxState was successful.\n");
    } else {
        printf("setMuxState returned 0x%08x.\n", kernResult);
    }
    return kernResult == KERN_SUCCESS;
}

static BOOL setFeatureInfo(io_connect_t connect, muxFeature feature, BOOL enabled) {
    return setMuxState(connect, enabled ? muxEnableFeature : muxDisableFeature, 1<<feature);
}

static BOOL getFeatureInfo(io_connect_t connect, muxFeature feature) {
    uint64_t featureInfo = 0;
    if (!getMuxState(connect, muxFeatureInfo, &featureInfo)) return 0;
    return (1<<feature) & featureInfo;
}

static void setSwitchPolicy(io_connect_t connect, BOOL dynamic) {
    // arg = 2: user needs to logout before switching, arg = 0: instant switching
    setMuxState(connect, muxSwitchPolicy, dynamic ? 0 : 2);
}

static void setDynamicSwitchingEnabled(io_connect_t connect, BOOL enabled) {
    // The same as clicking the checkbox in systemsettings.app
    setMuxState(connect, muxGpuSelect, enabled ? 1 : 0);
}

static void forceSwitch(io_connect_t connect) {
    // switch graphic cards now regardless of switching mode
    setMuxState(connect, muxForceSwitch, 0);
}

// --------------------------------------------------------------

static char *getFeatureName(muxFeature feature) {
    switch (feature) {
        case Policy: return "Policy";
        case Auto_PowerDown_GPU: return "Auto_PowerDown_GPU";
        case Dynamic_Switching: return "Dynamic_Switching";
        case GPU_Powerpolling: return "GPU_Powerpolling";
        case Defer_Policy: return "Defer_Policy";
        case Synchronous_Launch: return "Synchronous_Launch";
        case Backlight_Control: return "Backlight_Control";
        case Recovery_Timeouts: return "Recovery_Timeouts";
        case Power_Switch_Debounce: return "Power_Switch_Debounce";
        case Logging: return "Logging";
        case Display_Capture_Switch: return "Display_Capture_Switch";
        case No_GL_HDA_busy_idle_registration: return "No_GL_HDA_busy_idle_registration";
        default: return "Unknown Feature";
    }
}

static void printFeatures(io_connect_t connect) {
    uint64_t featureInfo = 0;
    getMuxState(connect, muxFeatureInfo, &featureInfo);
    muxFeature f;
    for (f = Policy; f < muxFeaturesCount; f++) {
        printf("%s: %s\n", getFeatureName(f), (featureInfo & (1<<f) ? "ON" : "OFF"));
    }
}

// ???
static void setExclusive(io_connect_t connect) {
    kern_return_t kernResult;
    uint64_t    scalarI_64[1];
    scalarI_64[0] = 0x0;
    
    kernResult = IOConnectCallScalarMethod(connect,       // an io_connect_t returned from IOServiceOpen().
                                           kSetExclusive, // selector of the function to be called via the user client.
                                           scalarI_64,    // array of scalar (64-bit) input values.
                                           1,             // the number of scalar input values.
                                           NULL,          // array of scalar (64-bit) output values.
                                           0);            // pointer to the number of scalar output values.
    
    if (kernResult == KERN_SUCCESS) {
        printf("setExclusive was successful.\n");
    } else {
        printf("setExclusive returned 0x%08x.\n", kernResult);
    }
}

typedef struct StateStruct {
    uint32_t field1[25]; // State Struct has to be 100 bytes long
} StateStruct;

static void dumpState(io_connect_t connect) {
    kern_return_t kernResult;
    StateStruct stateStruct;
    size_t structSize = sizeof(StateStruct);
    
    kernResult = IOConnectCallMethod(connect,      // an io_connect_t returned from IOServiceOpen().
                                     kDumpState,   // selector of the function to be called via the user client.
                                     NULL,         // array of scalar (64-bit) input values.
                                     0,            // the number of scalar input values.
                                     NULL,         // a pointer to the struct input parameter.
                                     0,            // the size of the input structure parameter.
                                     NULL,         // array of scalar (64-bit) output values.
                                     NULL,         // pointer to the number of scalar output values.
                                     &stateStruct, // pointer to the struct output parameter.
                                     &structSize); // pointer to the size of the output structure parameter.
    
    // TODO: figure the meaning of the values in StateStruct out
    
    if (kernResult == KERN_SUCCESS) {
        printf("setExclusive was successful.\n");
    } else {
        printf("setExclusive returned 0x%08x.\n", kernResult);
    }
}


int switcherOpen() {
    kern_return_t kernResult = 0; 
    io_service_t service = IO_OBJECT_NULL;
    io_iterator_t iterator = IO_OBJECT_NULL;
    
    // Look up the objects we wish to open.
    // This creates an io_iterator_t of all instances of our driver that exist in the I/O Registry.
    kernResult = IOServiceGetMatchingServices(kIOMasterPortDefault, IOServiceMatching(kDriverClassName), &iterator);    
    if (kernResult != KERN_SUCCESS) {
        printf("IOServiceGetMatchingServices returned 0x%08x.\n", kernResult);
        return 0;
    }
    
    service = IOIteratorNext(iterator); // actually there is only 1 such service
    IOObjectRelease(iterator);
    if (service == IO_OBJECT_NULL) {
        printf("No matching drivers found.\n");
        return 0;
    }
    
    // This call will cause the user client to be instantiated. It returns an io_connect_t handle
    // that is used for all subsequent calls to the user client.
    // Applications pass the bad-Bit (indicates they need the dedicated gpu here)
    // as uint32_t type, 0 = no dedicated gpu, 1 = dedicated
    kernResult = IOServiceOpen(service, mach_task_self(), 0, &switcherConnect);
    if (kernResult != KERN_SUCCESS) {
        printf("IOServiceOpen returned 0x%08x.\n", kernResult);
        return 0;
    }
    
    kernResult = IOConnectCallScalarMethod(switcherConnect, kOpen, NULL, 0, NULL, NULL);
    if (kernResult != KERN_SUCCESS) printf("IOConnectCallScalarMethod returned 0x%08x.\n", kernResult);
    else printf("Driver connection opened.\n");
    
    return kernResult == KERN_SUCCESS;
}

void switcherClose() {
    kern_return_t kernResult;
    if (switcherConnect == IO_OBJECT_NULL) return;
    
    kernResult = IOConnectCallScalarMethod(switcherConnect, kClose, NULL, 0, NULL, NULL);
    if (kernResult != KERN_SUCCESS) printf("IOConnectCallScalarMethod returned 0x%08x.\n", kernResult);
    
    kernResult = IOServiceClose(switcherConnect);
    if (kernResult != KERN_SUCCESS) printf("IOServiceClose returned 0x%08x.\n", kernResult);
    
    switcherConnect = IO_OBJECT_NULL;
    printf("Driver connection closed.");
}

int switcherSetMode(SwitcherMode mode) {
    if (switcherConnect == IO_OBJECT_NULL) return 0;
    switch (mode) {
        case modeForceIntegrated:
        case modeForceDiscrete:
            // Disable dynamic switching
            setDynamicSwitchingEnabled(switcherConnect, 0);
            
            // Disable Policy, otherwise gpu switches to Discrete after a bad app closes
            setFeatureInfo(switcherConnect, Policy, 0);
            setSwitchPolicy(switcherConnect, 0);
            
            // Hold up a sec!
            sleep(1);
            
            BOOL integrated = isUsingIntegrated();
            if ((mode==modeForceIntegrated && !integrated) || (mode==modeForceDiscrete && integrated))
                forceSwitch(switcherConnect);
            
            break;
        case modeDynamicSwitching:
            // Set switch policy back, make the MBP think it's an auto switching one once again
            setFeatureInfo(switcherConnect, Policy, 1);
            setSwitchPolicy(switcherConnect, 1);
            
            // Enable dynamic switching
            setDynamicSwitchingEnabled(switcherConnect, 1);
            
            break;
        case modeToggleGPU:
            forceSwitch(switcherConnect);
            
            break;
    }
    return 1;
}

int isUsingIntegrated() {
    uint64_t output;
    if (switcherConnect == IO_OBJECT_NULL) return 0;
    getMuxState(switcherConnect, muxGraphicsCard, &output);
    return output != 0;
}

int isUsingDynamicSwitching() {
    uint64_t output;
    if (switcherConnect == IO_OBJECT_NULL) return 0;
    getMuxState(switcherConnect, muxGpuSelect, &output);
    return output != 0;
}

/*void isUsingDebug() {*/
    /*uint64_t output;*/
    /*if (switcherConnect == IO_OBJECT_NULL) return;*/
    /*getMuxState(switcherConnect, 8, &output);*/
    /*printf("debug: %x\n", output);*/
/*}*/

#define REGISTER_COUNT 0x80
static BOOL dumpMuxRegisters(io_connect_t connect) {
    kern_return_t kernResult;
    uint8_t buffer[REGISTER_COUNT];
    uint64_t scalarI_64[2] = { (uint64_t) buffer, REGISTER_COUNT };
    
    kernResult = IOConnectCallMethod(connect,       // an io_connect_t returned from IOServiceOpen().
                                       kGetAGCData_log2,  // selector of the function to be called via the user client.
                                       scalarI_64,    // array of scalar (64-bit) input values.
                                       2,             // the number of scalar input values.
                                       NULL,          // a pointer to the struct input parameter.
                                       0,             // the size of the input structure parameter.
                                       NULL,          // array of scalar (64-bit) output values.
                                       NULL,          // pointer to the number of scalar output values.
                                       NULL,           // pointer to the struct output parameter.
                                       NULL // pointer to the size of the output structure parameter.
        );
    if (kernResult == KERN_SUCCESS) {
        printf("getMuxState was successful.\n");
        int i;
        for (i=0; i < REGISTER_COUNT; i++) {
            printf("0x%x: 0x%x\n", i, buffer[i]);
        }
    } else {
        printf("getMuxState returned 0x%08x.\n", kernResult);
    }
    return kernResult == KERN_SUCCESS;
}

// logging metadata, contains e.g. size of log1, pointer to current entry etc.
// 0xc == AGCDebug
static BOOL dumpLog0(io_connect_t connect) {
    kern_return_t kernResult;
    uint8_t buffer[0x100];
    uint64_t scalarI_64[2] = { (uint64_t) buffer, 0x100 };
    
    kernResult = IOConnectCallMethod(connect,       // an io_connect_t returned from IOServiceOpen().
                                       kGetAGCData,  // selector of the function to be called via the user client.
                                       scalarI_64,    // array of scalar (64-bit) input values.
                                       2,             // the number of scalar input values.
                                       NULL,          // a pointer to the struct input parameter.
                                       0,             // the size of the input structure parameter.
                                       NULL,          // array of scalar (64-bit) output values.
                                       NULL,          // pointer to the number of scalar output values.
                                       NULL,           // pointer to the struct output parameter.
                                       NULL // pointer to the size of the output structure parameter.
        );
    if (kernResult == KERN_SUCCESS) {
        printf("getMuxState was successful.\n");
        int i;
        for (i=0; i < 0x100; i++) {
            printf("0x%x: 0x%x\n", i, buffer[i]);
        }
    } else {
        printf("getMuxState returned 0x%08x.\n", kernResult);
    }
    return kernResult == KERN_SUCCESS;
}

// actual logging data
#define LOGSIZE 0x10000
static BOOL dumpLog1(io_connect_t connect) {
    kern_return_t kernResult;
    uint8_t buffer[LOGSIZE];
    uint64_t scalarI_64[2] = { (uint64_t) buffer, LOGSIZE };
    
    kernResult = IOConnectCallMethod(connect,       // an io_connect_t returned from IOServiceOpen().
                                       kGetAGCData_log1,  // selector of the function to be called via the user client.
                                       scalarI_64,    // array of scalar (64-bit) input values.
                                       2,             // the number of scalar input values.
                                       NULL,          // a pointer to the struct input parameter.
                                       0,             // the size of the input structure parameter.
                                       NULL,          // array of scalar (64-bit) output values.
                                       NULL,          // pointer to the number of scalar output values.
                                       NULL,           // pointer to the struct output parameter.
                                       NULL // pointer to the size of the output structure parameter.
        );
    if (kernResult == KERN_SUCCESS) {
        printf("getMuxState was successful.\n");
        FILE *logfile;
        logfile = fopen("log.bin", "wb");
        if (!logfile) {
            printf("cannot open file\n");
        } else {
            fwrite(buffer, sizeof(uint8_t), LOGSIZE, logfile);
            fclose(logfile);
        }

    } else {
        printf("getMuxState returned 0x%08x.\n", kernResult);
    }
    return kernResult == KERN_SUCCESS;
}

int main() {
    switcherOpen();
    /*printFeatures(switcherConnect);*/
    setMuxState(switcherConnect, muxDebug, 0xefffffff);
    uint64_t debug = 0;
    getMuxState(switcherConnect, muxDebug, &debug);
    // enable logging
    setMuxState(switcherConnect, muxEnableFeature, 0x10000);
    /*dumpMuxRegisters(switcherConnect);*/
    /*dumpLog0(switcherConnect);*/
    /*dumpLog1(switcherConnect);*/
    /*isUsingDebug();*/
    switcherClose();
    return 0;
}
