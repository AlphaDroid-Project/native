/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "Sensors"

#include <sensor/SensorManager.h>

#include <stdint.h>
#include <sys/types.h>

#include <cutils/native_handle.h>
#include <utils/Errors.h>
#include <utils/RefBase.h>
#include <utils/Singleton.h>

#include <android/companion/virtualnative/IVirtualDeviceManagerNative.h>

#include <binder/IBinder.h>
#include <binder/IPermissionController.h>
#include <binder/IServiceManager.h>

#include <sensor/ISensorServer.h>
#include <sensor/ISensorEventConnection.h>
#include <sensor/Sensor.h>
#include <sensor/SensorEventQueue.h>

#include <com_android_hardware_libsensor_flags.h>
namespace libsensor_flags = com::android::hardware::libsensor::flags;

// ----------------------------------------------------------------------------
namespace android {
// ----------------------------------------------------------------------------

namespace {

using ::android::companion::virtualnative::IVirtualDeviceManagerNative;

static constexpr int DEVICE_ID_DEFAULT = 0;

// Returns the deviceId of the device where this uid is observed. If the uid is present on more than
// one devices, return the default deviceId.
int getDeviceIdForUid(uid_t uid) {
    sp<IBinder> binder =
            defaultServiceManager()->checkService(String16("virtualdevice_native"));
    if (binder != nullptr) {
        auto vdm = interface_cast<IVirtualDeviceManagerNative>(binder);
        std::vector<int> deviceIds;
        vdm->getDeviceIdsForUid(uid, &deviceIds);
        // If the UID is associated with multiple virtual devices, use the default device's
        // sensors as we cannot disambiguate here. This effectively means that the app has
        // activities on different devices at the same time, so it must handle the device
        // awareness by itself.
        if (deviceIds.size() == 1) {
            const int deviceId = deviceIds.at(0);
            int devicePolicy = IVirtualDeviceManagerNative::DEVICE_POLICY_DEFAULT;
            vdm->getDevicePolicy(deviceId,
                                 IVirtualDeviceManagerNative::POLICY_TYPE_SENSORS,
                                 &devicePolicy);
            if (devicePolicy == IVirtualDeviceManagerNative::DEVICE_POLICY_CUSTOM) {
                return deviceId;
            }
        }
    }
    return DEVICE_ID_DEFAULT;
}

bool findSensorNameInList(int32_t handle, const Vector<Sensor>& sensorList,
                          std::string* outString) {
    for (auto& sensor : sensorList) {
        if (sensor.getHandle() == handle) {
            std::ostringstream oss;
            oss << sensor.getStringType() << ":" << sensor.getName();
            if (outString) {
                *outString = oss.str();
            }
            return true;
        }
    }
    return false;
}

}  // namespace

Mutex SensorManager::sLock;
std::map<String16, SensorManager*> SensorManager::sPackageInstances;

SensorManager& SensorManager::getInstanceForPackage(const String16& packageName) {
    waitForSensorService(nullptr);

    Mutex::Autolock _l(sLock);
    SensorManager* sensorManager;
    auto iterator = sPackageInstances.find(packageName);

    const uid_t uid = IPCThreadState::self()->getCallingUid();
    const int deviceId = getDeviceIdForUid(uid);

    // Return the cached instance if the device association of the package has not changed.
    if (iterator != sPackageInstances.end()) {
        sensorManager = iterator->second;
        if (sensorManager->mDeviceId == deviceId) {
            return *sensorManager;
        }
    }

    // It is possible that the calling code has no access to the package name.
    // In this case we will get the packages for the calling UID and pick the
    // first one for attributing the app op. This will work correctly for
    // runtime permissions as for legacy apps we will toggle the app op for
    // all packages in the UID. The caveat is that the operation may be attributed
    // to the wrong package and stats based on app ops may be slightly off.
    String16 opPackageName = packageName;
    if (opPackageName.size() <= 0) {
        sp<IBinder> binder = defaultServiceManager()->getService(String16("permission"));
        if (binder != nullptr) {
            Vector<String16> packages;
            interface_cast<IPermissionController>(binder)->getPackagesForUid(uid, packages);
            if (!packages.isEmpty()) {
                opPackageName = packages[0];
            } else {
                ALOGE("No packages for calling UID");
            }
        } else {
            ALOGE("Cannot get permission service");
        }
    }

    sensorManager = new SensorManager(opPackageName, deviceId);

    // If we had no package name, we looked it up from the UID and the sensor
    // manager instance we created should also be mapped to the empty package
    // name, to avoid looking up the packages for a UID and get the same result.
    if (packageName.size() <= 0) {
        sPackageInstances.insert(std::make_pair(String16(), sensorManager));
    }

    // Stash the per package sensor manager.
    sPackageInstances.insert(std::make_pair(opPackageName, sensorManager));

    return *sensorManager;
}

void SensorManager::removeInstanceForPackage(const String16& packageName) {
    Mutex::Autolock _l(sLock);
    auto iterator = sPackageInstances.find(packageName);
    if (iterator != sPackageInstances.end()) {
        SensorManager* sensorManager = iterator->second;
        delete sensorManager;
        sPackageInstances.erase(iterator);
    }
}

SensorManager::SensorManager(const String16& opPackageName, int deviceId)
    : mSensorList(nullptr), mOpPackageName(opPackageName), mDeviceId(deviceId),
        mDirectConnectionHandle(1) {
    Mutex::Autolock _l(mLock);
    assertStateLocked();
}

SensorManager::~SensorManager() {
    free(mSensorList);
    free(mDynamicSensorList);
}

status_t SensorManager::waitForSensorService(sp<ISensorServer> *server) {
    // try for 300 seconds (60*5(getService() tries for 5 seconds)) before giving up ...
    sp<ISensorServer> s;
    const String16 name("sensorservice");
    for (int i = 0; i < 60; i++) {
        status_t err = getService(name, &s);
        switch (err) {
            case NAME_NOT_FOUND:
                sleep(1);
                continue;
            case NO_ERROR:
                if (server != nullptr) {
                    *server = s;
                }
                return NO_ERROR;
            default:
                return err;
        }
    }
    return TIMED_OUT;
}

void SensorManager::sensorManagerDied() {
    Mutex::Autolock _l(mLock);
    mSensorServer.clear();
    free(mSensorList);
    mSensorList = nullptr;
    mSensors.clear();
    free(mDynamicSensorList);
    mDynamicSensorList = nullptr;
    mDynamicSensors.clear();
}

status_t SensorManager::assertStateLocked() {
#if COM_ANDROID_HARDWARE_LIBSENSOR_FLAGS(SENSORMANAGER_PING_BINDER)
    if (mSensorServer == nullptr) {
#else
    bool initSensorManager = false;
    if (mSensorServer == nullptr) {
        initSensorManager = true;
    } else {
        // Ping binder to check if sensorservice is alive.
        status_t err = IInterface::asBinder(mSensorServer)->pingBinder();
        if (err != NO_ERROR) {
            initSensorManager = true;
        }
    }
    if (initSensorManager) {
#endif
        waitForSensorService(&mSensorServer);
        LOG_ALWAYS_FATAL_IF(mSensorServer == nullptr, "getService(SensorService) NULL");

        class DeathObserver : public IBinder::DeathRecipient {
            SensorManager& mSensorManager;
            virtual void binderDied(const wp<IBinder>& who) {
                ALOGW("sensorservice died [%p]", static_cast<void*>(who.unsafe_get()));
                mSensorManager.sensorManagerDied();
            }
        public:
            explicit DeathObserver(SensorManager& mgr) : mSensorManager(mgr) { }
        };

        mDeathObserver = new DeathObserver(*const_cast<SensorManager *>(this));
        IInterface::asBinder(mSensorServer)->linkToDeath(mDeathObserver);

        if (mDeviceId == DEVICE_ID_DEFAULT) {
            mSensors = mSensorServer->getSensorList(mOpPackageName);
        } else {
            mSensors = mSensorServer->getRuntimeSensorList(mOpPackageName, mDeviceId);
        }

        size_t count = mSensors.size();
        // If count is 0, mSensorList will be non-null. This is old
        // existing behavior and callers expect this.
        mSensorList =
                static_cast<Sensor const**>(malloc(count * sizeof(Sensor*)));
        LOG_ALWAYS_FATAL_IF(mSensorList == nullptr, "mSensorList NULL");

        for (size_t i=0 ; i<count ; i++) {
            mSensorList[i] = mSensors.array() + i;
        }
    }

    return NO_ERROR;
}

ssize_t SensorManager::getSensorList(Sensor const* const** list) {
    Mutex::Autolock _l(mLock);
    status_t err = assertStateLocked();
    if (err < 0) {
        return static_cast<ssize_t>(err);
    }
    *list = mSensorList;
    return static_cast<ssize_t>(mSensors.size());
}

ssize_t SensorManager::getDefaultDeviceSensorList(Vector<Sensor> & list) {
    Mutex::Autolock _l(mLock);
    status_t err = assertStateLocked();
    if (err < 0) {
        return static_cast<ssize_t>(err);
    }

    if (mDeviceId == DEVICE_ID_DEFAULT) {
        list = mSensors;
    } else {
        list = mSensorServer->getSensorList(mOpPackageName);
    }

    return static_cast<ssize_t>(list.size());
}

ssize_t SensorManager::getDynamicSensorList(Vector<Sensor> & dynamicSensors) {
    Mutex::Autolock _l(mLock);
    status_t err = assertStateLocked();
    if (err < 0) {
        return static_cast<ssize_t>(err);
    }

    dynamicSensors = mSensorServer->getDynamicSensorList(mOpPackageName);
    size_t count = dynamicSensors.size();

    return static_cast<ssize_t>(count);
}

ssize_t SensorManager::getRuntimeSensorList(int deviceId, Vector<Sensor>& runtimeSensors) {
    Mutex::Autolock _l(mLock);
    status_t err = assertStateLocked();
    if (err < 0) {
        return static_cast<ssize_t>(err);
    }

    runtimeSensors = mSensorServer->getRuntimeSensorList(mOpPackageName, deviceId);
    size_t count = runtimeSensors.size();

    return static_cast<ssize_t>(count);
}

ssize_t SensorManager::getDynamicSensorList(Sensor const* const** list) {
    Mutex::Autolock _l(mLock);
    status_t err = assertStateLocked();
    if (err < 0) {
        return static_cast<ssize_t>(err);
    }

    free(mDynamicSensorList);
    mDynamicSensorList = nullptr;
    mDynamicSensors = mSensorServer->getDynamicSensorList(mOpPackageName);
    size_t dynamicCount = mDynamicSensors.size();
    if (dynamicCount > 0) {
        mDynamicSensorList = static_cast<Sensor const**>(
                malloc(dynamicCount * sizeof(Sensor*)));
        if (mDynamicSensorList == nullptr) {
          ALOGE("Failed to allocate dynamic sensor list for %zu sensors.",
                dynamicCount);
          return static_cast<ssize_t>(NO_MEMORY);
        }

        for (size_t i = 0; i < dynamicCount; i++) {
            mDynamicSensorList[i] = mDynamicSensors.array() + i;
        }
    }

    *list = mDynamicSensorList;
    return static_cast<ssize_t>(mDynamicSensors.size());
}

Sensor const* SensorManager::getDefaultSensor(int type)
{
    Mutex::Autolock _l(mLock);
    if (assertStateLocked() == NO_ERROR) {
        bool wakeUpSensor = false;
        // For the following sensor types, return a wake-up sensor. These types are by default
        // defined as wake-up sensors. For the rest of the sensor types defined in sensors.h return
        // a non_wake-up version.
        if (type == SENSOR_TYPE_PROXIMITY || type == SENSOR_TYPE_SIGNIFICANT_MOTION ||
            type == SENSOR_TYPE_TILT_DETECTOR || type == SENSOR_TYPE_WAKE_GESTURE ||
            type == SENSOR_TYPE_GLANCE_GESTURE || type == SENSOR_TYPE_PICK_UP_GESTURE ||
            type == SENSOR_TYPE_WRIST_TILT_GESTURE ||
            type == SENSOR_TYPE_LOW_LATENCY_OFFBODY_DETECT || type == SENSOR_TYPE_HINGE_ANGLE) {
            wakeUpSensor = true;
        }
        // For now we just return the first sensor of that type we find.
        // in the future it will make sense to let the SensorService make
        // that decision.
        for (size_t i=0 ; i<mSensors.size() ; i++) {
            if (mSensorList[i]->getType() == type &&
                mSensorList[i]->isWakeUpSensor() == wakeUpSensor) {
                return mSensorList[i];
            }
        }
    }
    return nullptr;
}

std::optional<std::string_view> SensorManager::getSensorNameByHandle(int32_t handle) {
    std::lock_guard<std::mutex> lock(mSensorHandleToNameMutex);
    auto iterator = mSensorHandleToName.find(handle);
    if (iterator != mSensorHandleToName.end()) {
        return iterator->second;
    }

    std::string sensorName;
    if (!findSensorNameInList(handle, mSensors, &sensorName) &&
        !findSensorNameInList(handle, mDynamicSensors, &sensorName)) {
        ALOGW("Cannot find sensor with handle %d", handle);
        return std::nullopt;
    }

    mSensorHandleToName[handle] = std::move(sensorName);

    return mSensorHandleToName[handle];
}

sp<SensorEventQueue> SensorManager::createEventQueue(
    String8 packageName, int mode, String16 attributionTag) {
    sp<SensorEventQueue> queue;

    Mutex::Autolock _l(mLock);
    while (assertStateLocked() == NO_ERROR) {
        sp<ISensorEventConnection> connection = mSensorServer->createSensorEventConnection(
            packageName, mode, mOpPackageName, attributionTag);
        if (connection == nullptr) {
            // SensorService just died or the app doesn't have required permissions.
            ALOGE("createEventQueue: connection is NULL.");
            return nullptr;
        }
        queue = new SensorEventQueue(connection, *this, packageName);
        break;
    }
    return queue;
}

bool SensorManager::isDataInjectionEnabled() {
    Mutex::Autolock _l(mLock);
    if (assertStateLocked() == NO_ERROR) {
        return mSensorServer->isDataInjectionEnabled();
    }
    return false;
}

bool SensorManager::isReplayDataInjectionEnabled() {
    Mutex::Autolock _l(mLock);
    if (assertStateLocked() == NO_ERROR) {
        return mSensorServer->isReplayDataInjectionEnabled();
    }
    return false;
}

bool SensorManager::isHalBypassReplayDataInjectionEnabled() {
    Mutex::Autolock _l(mLock);
    if (assertStateLocked() == NO_ERROR) {
        return mSensorServer->isHalBypassReplayDataInjectionEnabled();
    }
    return false;
}

int SensorManager::createDirectChannel(
        size_t size, int channelType, const native_handle_t *resourceHandle) {
    static constexpr int DEFAULT_DEVICE_ID = 0;
    return createDirectChannel(DEFAULT_DEVICE_ID, size, channelType, resourceHandle);
}

int SensorManager::createDirectChannel(
        int deviceId, size_t size, int channelType, const native_handle_t *resourceHandle) {
    Mutex::Autolock _l(mLock);
    if (assertStateLocked() != NO_ERROR) {
        return NO_INIT;
    }

    if (channelType != SENSOR_DIRECT_MEM_TYPE_ASHMEM
            && channelType != SENSOR_DIRECT_MEM_TYPE_GRALLOC) {
        ALOGE("Bad channel shared memory type %d", channelType);
        return BAD_VALUE;
    }

    sp<ISensorEventConnection> conn =
              mSensorServer->createSensorDirectConnection(mOpPackageName, deviceId,
                  static_cast<uint32_t>(size),
                  static_cast<int32_t>(channelType),
                  SENSOR_DIRECT_FMT_SENSORS_EVENT, resourceHandle);
    if (conn == nullptr) {
        return NO_MEMORY;
    }

    int nativeHandle = mDirectConnectionHandle++;
    mDirectConnection.emplace(nativeHandle, conn);
    return nativeHandle;
}

void SensorManager::destroyDirectChannel(int channelNativeHandle) {
    Mutex::Autolock _l(mLock);
    if (assertStateLocked() == NO_ERROR) {
        mDirectConnection.erase(channelNativeHandle);
    }
}

int SensorManager::configureDirectChannel(int channelNativeHandle, int sensorHandle, int rateLevel) {
    Mutex::Autolock _l(mLock);
    if (assertStateLocked() != NO_ERROR) {
        return NO_INIT;
    }

    auto i = mDirectConnection.find(channelNativeHandle);
    if (i == mDirectConnection.end()) {
        ALOGE("Cannot find the handle in client direct connection table");
        return BAD_VALUE;
    }

    int ret;
    ret = i->second->configureChannel(sensorHandle, rateLevel);
    ALOGE_IF(ret < 0, "SensorManager::configureChannel (%d, %d) returns %d",
            static_cast<int>(sensorHandle), static_cast<int>(rateLevel),
            static_cast<int>(ret));
    return ret;
}

int SensorManager::setOperationParameter(
        int handle, int type,
        const Vector<float> &floats, const Vector<int32_t> &ints) {
    Mutex::Autolock _l(mLock);
    if (assertStateLocked() != NO_ERROR) {
        return NO_INIT;
    }
    return mSensorServer->setOperationParameter(handle, type, floats, ints);
}

// ----------------------------------------------------------------------------
}; // namespace android
