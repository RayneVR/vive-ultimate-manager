#include <openvr_driver.h>
#include "ultimate_tracker.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace {

struct TrackerRecord {
    std::string name;
    vive::TrackerData data{};
    bool hasData = false;
};

std::mutex g_mutex;
std::map<uint32_t, TrackerRecord> g_trackers;

void Log(const std::string& message) {
    if (vr::VRDriverLog())
        vr::VRDriverLog()->Log(message.c_str());
}

// Convert a rotation matrix to an OpenVR quaternion.
void MatrixToQuaternion(const double m[3][3], double& w,
                        double& x, double& y, double& z) {
    const double trace = m[0][0] + m[1][1] + m[2][2];

    if (trace > 0.0) {
        const double s = std::sqrt(trace + 1.0) * 2.0;
        w = 0.25 * s;
        x = (m[2][1] - m[1][2]) / s;
        y = (m[0][2] - m[2][0]) / s;
        z = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const double s = std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2.0;
        w = (m[2][1] - m[1][2]) / s;
        x = 0.25 * s;
        y = (m[0][1] + m[1][0]) / s;
        z = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        const double s = std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2.0;
        w = (m[0][2] - m[2][0]) / s;
        x = (m[0][1] + m[1][0]) / s;
        y = 0.25 * s;
        z = (m[1][2] + m[2][1]) / s;
    } else {
        const double s = std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2.0;
        w = (m[1][0] - m[0][1]) / s;
        x = (m[0][2] + m[2][0]) / s;
        y = (m[1][2] + m[2][1]) / s;
        z = 0.25 * s;
    }
}

void ConvertRotation(const float q[4], vr::HmdQuaternion_t& out) {
    // HTC SDK quaternion order: x, y, z, w.
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double n = std::sqrt(w*w + x*x + y*y + z*z);

    if (n < 1e-8) {
        out = {1.0, 0.0, 0.0, 0.0};
        return;
    }

    const double qw = w/n, qx = x/n, qy = y/n, qz = z/n;

    // Rotation matrix in HTC coordinates.
    double r[3][3] = {
        {1-2*(qy*qy+qz*qz), 2*(qx*qy-qz*qw),   2*(qx*qz+qy*qw)},
        {2*(qx*qy+qz*qw),   1-2*(qx*qx+qz*qz), 2*(qy*qz-qx*qw)},
        {2*(qx*qz-qy*qw),   2*(qy*qz+qx*qw),   1-2*(qx*qx+qy*qy)}
    };

    // HTC (forward, left, up) -> OpenVR (right, up, back).
    // C = [ 0 -1  0; 0  0  1; -1  0  0 ]
    // R_openvr = C * R_htc * transpose(C)
    const int c[3][3] = {{0,-1,0},{0,0,1},{-1,0,0}};
    double temp[3][3]{}, result[3][3]{};

    for (int i=0;i<3;i++)
        for (int j=0;j<3;j++)
            for (int k=0;k<3;k++)
                temp[i][j] += c[i][k] * r[k][j];

    for (int i=0;i<3;i++)
        for (int j=0;j<3;j++)
            for (int k=0;k<3;k++)
                result[i][j] += temp[i][k] * c[j][k];

    MatrixToQuaternion(result, out.w, out.x, out.y, out.z);
}

class TrackerDevice final : public vr::ITrackedDeviceServerDriver {
public:
    explicit TrackerDevice(uint32_t sdkId, std::string serial)
        : sdkId_(sdkId), serial_(std::move(serial)) {}

    vr::EVRInitError Activate(uint32_t objectId) override {
        objectId_ = objectId;
        container_ = vr::VRProperties()->TrackedDeviceToPropertyContainer(objectId_);

        auto* props = vr::VRProperties();
        props->SetStringProperty(container_, vr::Prop_TrackingSystemName_String,
                                 "ViveUltimate");
        props->SetStringProperty(container_, vr::Prop_ModelNumber_String,
                                 "VIVE Ultimate Tracker");
        // Official VIVE Ultimate Tracker SteamVR render model (resource directory below).
        props->SetStringProperty(container_, vr::Prop_RenderModelName_String,
                                 "{vive_ultimate}vive_ultimate_tracker");
        props->SetStringProperty(container_, vr::Prop_NamedIconPathDeviceReady_String,
                                 "{vive_ultimate}/icons/tracker_status_ready.png");
        props->SetStringProperty(container_, vr::Prop_NamedIconPathDeviceOff_String,
                                 "{vive_ultimate}/icons/tracker_status_off_dark.png");
        props->SetStringProperty(container_, vr::Prop_NamedIconPathDeviceSearching_String,
                                 "{vive_ultimate}/icons/tracker_status_searching.gif");
        props->SetStringProperty(container_, vr::Prop_NamedIconPathDeviceAlertLow_String,
                                 "{vive_ultimate}/icons/tracker_status_ready_low.png");
        props->SetStringProperty(container_, vr::Prop_SerialNumber_String,
                                 serial_.c_str());
        props->SetStringProperty(container_, vr::Prop_ManufacturerName_String,
                                 "HTC");
        props->SetBoolProperty(container_, vr::Prop_DeviceIsWireless_Bool, true);
        props->SetBoolProperty(container_, vr::Prop_WillDriftInYaw_Bool, false);
        props->SetInt32Property(container_, vr::Prop_ControllerRoleHint_Int32,
                                vr::TrackedControllerRole_Invalid);

        active_ = true;
        Log("Activated VIVE Ultimate Tracker: " + serial_);
        return vr::VRInitError_None;
    }

    void Deactivate() override {
        active_ = false;
        objectId_ = vr::k_unTrackedDeviceIndexInvalid;
    }

    void EnterStandby() override {}
    void* GetComponent(const char*) override { return nullptr; }

    void DebugRequest(const char*, char* response, uint32_t responseSize) override {
        if (response && responseSize)
            response[0] = '\0';
    }

    vr::DriverPose_t GetPose() override {
        std::lock_guard<std::mutex> lock(poseMutex_);
        return pose_;
    }

    void UpdatePose(const vive::TrackerData& d) {
        vr::DriverPose_t p{};
        p.poseTimeOffset = 0.0;
        p.qWorldFromDriverRotation = {1.0, 0.0, 0.0, 0.0};
        p.vecWorldFromDriverTranslation[0] = 0.0;
        p.vecWorldFromDriverTranslation[1] = 0.0;
        p.vecWorldFromDriverTranslation[2] = 0.0;
        p.qDriverFromHeadRotation = {1.0, 0.0, 0.0, 0.0};
        p.vecDriverFromHeadTranslation[0] = 0.0;
        p.vecDriverFromHeadTranslation[1] = 0.0;
        p.vecDriverFromHeadTranslation[2] = 0.0;

        // Position/velocity: HTC forward-left-up -> OpenVR right-up-back.
        p.vecPosition[0] = -d.Location[1];
        p.vecPosition[1] =  d.Location[2];
        p.vecPosition[2] = -d.Location[0];

        p.vecVelocity[0] = -d.Velocity[1];
        p.vecVelocity[1] =  d.Velocity[2];
        p.vecVelocity[2] = -d.Velocity[0];

        p.vecAngularVelocity[0] = -d.AngularVelocity[1];
        p.vecAngularVelocity[1] =  d.AngularVelocity[2];
        p.vecAngularVelocity[2] = -d.AngularVelocity[0];

        ConvertRotation(d.Rotation, p.qRotation);

        p.result = vr::TrackingResult_Running_OK;
        p.poseIsValid = true;
        p.deviceIsConnected = true;
        p.willDriftInYaw = false;
        p.shouldApplyHeadModel = false;

        {
            std::lock_guard<std::mutex> lock(poseMutex_);
            pose_ = p;
        }
        if (active_)
            vr::VRServerDriverHost()->TrackedDevicePoseUpdated(
                objectId_, p, sizeof(p));
    }

    uint32_t SdkId() const { return sdkId_; }

private:
    uint32_t sdkId_;
    std::string serial_;
    uint32_t objectId_ = vr::k_unTrackedDeviceIndexInvalid;
    vr::PropertyContainerHandle_t container_ = 0;
    bool active_ = false;
    std::mutex poseMutex_;
    vr::DriverPose_t pose_{};
};

class TrackerSubscriber final : public vive::ITrackerSubscriber {
public:
    void ServerStart() override { Log("HTC tracker server started"); }
    void ServerStop() override { Log("HTC tracker server stopped"); }

    void Add(uint32_t id, char const* name) override {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto& t = g_trackers[id];
        t.name = (name && *name) ? name : ("VIVE-ULT-" + std::to_string(id));
        Log("SDK tracker discovered: " + t.name);
    }

    void Update(vive::TrackerData const& data) override {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto& t = g_trackers[data.Id];
        if (t.name.empty())
            t.name = "VIVE-ULT-" + std::to_string(data.Id);
        t.data = data;
        t.hasData = true;
    }

    void Click(uint32_t, int, int) override {}
    void Remove(uint32_t id) override {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_trackers.erase(id);
    }
    void CommandResponse(vive::TrackerCommandResponse const&) override {}
};

class TrackerProvider final : public vr::IServerTrackedDeviceProvider {
public:
    vr::EVRInitError Init(vr::IVRDriverContext* context) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(context);
        Log("VIVE Ultimate SteamVR driver initializing");
        Log("Calling vive::StartTrackerServer()");
        const bool started = vive::StartTrackerServer();
        Log(std::string("StartTrackerServer returned: ") +
            (started ? "TRUE" : "FALSE"));
        return vr::VRInitError_None;
    }

    void Cleanup() override {
        vive::StopTrackerServer();
        devices_.clear();
        VR_CLEANUP_SERVER_DRIVER_CONTEXT();
    }

    const char* const* GetInterfaceVersions() override {
        return vr::k_InterfaceVersions;
    }

    void RunFrame() override {
        std::map<uint32_t, TrackerRecord> snapshot;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            snapshot = g_trackers;
        }

        for (const auto& [id, tracker] : snapshot) {
            auto it = devices_.find(id);

            if (it == devices_.end()) {
                std::string serial = tracker.name.empty()
                    ? ("VIVE-ULT-" + std::to_string(id)) : tracker.name;

                auto device = std::make_unique<TrackerDevice>(id, serial);
                if (vr::VRServerDriverHost()->TrackedDeviceAdded(
                        serial.c_str(), vr::TrackedDeviceClass_GenericTracker,
                        device.get())) {
                    Log("Registered tracker " + serial);
                    it = devices_.emplace(id, std::move(device)).first;
                } else {
                    Log("SteamVR rejected tracker " + serial);
                    continue;
                }
            }

            if (tracker.hasData && static_cast<int>(tracker.data.State) >= 8)
                it->second->UpdatePose(tracker.data);
        }
    }

    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}

private:
    std::map<uint32_t, std::unique_ptr<TrackerDevice>> devices_;
};

TrackerProvider g_provider;
TrackerSubscriber g_subscriber;

} // namespace

extern "C" __attribute__((visibility("default")))
void* HmdDriverFactory(const char* interfaceName, int* returnCode) {
    if (interfaceName &&
        std::strcmp(interfaceName, vr::IServerTrackedDeviceProvider_Version) == 0) {
        if (returnCode) *returnCode = vr::VRInitError_None;
        return &g_provider;
    }

    if (returnCode)
        *returnCode = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
