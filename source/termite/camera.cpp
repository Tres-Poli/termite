#include "pch.h"
#include "camera.h"

using namespace tee;

void tee::camInit(Camera* cam, float fov /*= 60.0f*/, float fnear /*= 0.1f*/, float ffar /*= 100.0f*/)
{
    bx::memSet(cam, 0x00, sizeof(Camera));
    
    cam->right = vec3(1.0f, 0, 0);
    cam->up = vec3(0, 1.0f, 0);
    cam->forward = vec3(0, 0, 1.0f);
    
    cam->fov = fov;
    cam->fnear = fnear;
    cam->ffar = ffar;
    cam->quat = quaternionI();
}

void tee::camLookAt(Camera* cam, const vec3_t pos, const vec3_t lookat)
{
    vec3_t forward = lookat - pos;
    bx::vec3Norm(forward.f, forward.f);

    vec3_t right;
    bx::vec3Cross(right.f, vec3(0, 1.0f, 0).f, forward.f);
    bx::vec3Norm(right.f, right.f);

    vec3_t up;
    bx::vec3Cross(up.f, forward.f, right.f);

    mat4_t m = mat4f3(right.f, up.f, forward.f, vec3(0, 0, 0).f);

    cam->forward = forward;
    cam->right = right;
    cam->up = up;
    cam->pos = pos;
    bx::quatMtx(cam->quat.f, m.f);    

    float euler[3];
    bx::quatToEuler(euler, cam->quat.f);
    cam->pitch = euler[0];
    cam->yaw = euler[1];
}

void tee::camCalcFrustumCorners(const Camera* cam, vec3_t result[8], float aspectRatio, 
                                    float nearOverride /*= 0*/, float farOverride /*= 0*/)
{
    const float ffar = farOverride != 0.0f ? farOverride : cam->ffar;
    const float fnear = nearOverride != 0.0f ? nearOverride : cam->fnear;
    const float fov = bx::toRad(cam->fov);

    vec3_t xaxis = cam->right;
    vec3_t yaxis = cam->up;
    vec3_t zaxis = cam->forward;

    float nearPlaneHeight = bx::tan(fov * 0.5f) * fnear;
    float nearPlaneWidth = nearPlaneHeight * aspectRatio;

    float farPlaneHeight = bx::tan(fov * 0.5f) * ffar;
    float farPlaneWidth = farPlaneHeight * aspectRatio;

    // Far/Near planes
    vec3_t centerNear = zaxis*fnear + cam->pos;
    vec3_t centerFar = zaxis*ffar + cam->pos;

    // Scaled axises
    vec3_t xNearScaled = xaxis*nearPlaneWidth;
    vec3_t xFarScaled = xaxis*farPlaneWidth;
    vec3_t yNearScaled = yaxis*nearPlaneHeight;
    vec3_t yFarScaled = yaxis*farPlaneHeight;

    // Near quad
    result[0] = centerNear - (xNearScaled + yNearScaled);
    result[1] = centerNear - (xNearScaled - yNearScaled);
    result[2] = centerNear + (xNearScaled + yNearScaled);
    result[3] = centerNear + (xNearScaled - yNearScaled);

    // Far quad
    result[4] = centerFar - (xFarScaled + yFarScaled);
    result[5] = centerFar - (xFarScaled - yFarScaled);
    result[6] = centerFar + (xFarScaled + yFarScaled);
    result[7] = centerFar + (xFarScaled - yFarScaled);
}

void tee::camCalcFrustumPlanes(plane_t result[CameraPlane::Count], const mat4_t& viewProjMtx)
{
    const mat4_t vp = viewProjMtx;
    result[0] = plane(vp.m14 + vp.m11, vp.m24 + vp.m21, vp.m34 + vp.m31, vp.m44 + vp.m41);
    result[1] = plane(vp.m14 - vp.m11, vp.m24 - vp.m21, vp.m34 - vp.m31, vp.m44 - vp.m41);
    result[2] = plane(vp.m14 - vp.m12, vp.m24 - vp.m22, vp.m34 - vp.m32, vp.m44 - vp.m42);
    result[3] = plane(vp.m14 + vp.m12, vp.m24 + vp.m22, vp.m34 + vp.m32, vp.m44 + vp.m42);
    result[4] = plane(vp.m13, vp.m23, vp.m33, vp.m43);
    result[5] = plane(vp.m14 - vp.m13, vp.m24 - vp.m23, vp.m34 - vp.m33, vp.m44 - vp.m43);

    // Normalize result
    for (int i = 0; i < int(CameraPlane::Count); i++) {
        plane_t& p = result[i];
        vec3_t nd = vec3(p.nx, p.ny, p.nz);
        
        float nlen = bx::vec3Length(nd.f);
        p.nx /= nlen;
        p.ny /= nlen;
        p.nz /= nlen;
        p.d /= nlen;
    }
}

static void updateRotation(Camera* cam)
{
    mat4_t m;
    bx::mtxQuat(m.f, cam->quat.f);
    cam->right = vec3(m.m11, m.m12, m.m13);
    cam->up = vec3(m.m21, m.m22, m.m23);
    cam->forward = vec3(m.m31, m.m32, m.m33);
}

void tee::camPitch(Camera* cam, float pitch)
{
    cam->pitch += pitch;
    quat_t q1, q2;
    bx::quatRotateAxis(q1.f, vec3(0, 1.0f, 0).f, cam->yaw);
    bx::quatRotateAxis(q2.f, vec3(1.0f, 0, 0).f, cam->pitch);
    bx::quatMul(cam->quat.f, q2.f, q1.f);

    updateRotation(cam);
}

void tee::camYaw(Camera* cam, float yaw)
{
    cam->yaw += yaw;

    quat_t q1, q2;
    bx::quatRotateAxis(q1.f, vec3(0, 1.0f, 0).f, cam->yaw);
    bx::quatRotateAxis(q2.f, vec3(1.0f, 0, 0).f, cam->pitch);
    bx::quatMul(cam->quat.f, q2.f, q1.f);

    updateRotation(cam);
}

void tee::camPitchYaw(Camera* cam, float pitch, float yaw)
{
    cam->pitch += pitch;
    cam->yaw += yaw;

    quat_t q1, q2;
    bx::quatRotateAxis(q1.f, vec3(0, 1.0f, 0).f, cam->yaw);
    bx::quatRotateAxis(q2.f, vec3(1.0f, 0, 0).f, cam->pitch);
    bx::quatMul(cam->quat.f, q2.f, q1.f);

    updateRotation(cam);
}

void tee::camRoll(Camera* cam, float roll)
{
    quat_t q;
    bx::quatRotateAxis(q.f, vec3(0, 0, 1.0f).f, roll);
    quat_t qa = cam->quat;
    bx::quatMul(cam->quat.f, qa.f, q.f);

    updateRotation(cam);
}

void tee::camForward(Camera* cam, float fwd)
{
    cam->pos = cam->pos + cam->forward*fwd;
}

void tee::camStrafe(Camera* cam, float strafe)
{
    cam->pos = cam->pos + cam->right*strafe;
}

mat4_t tee::camViewMtx(const Camera* cam)
{
    const vec3_t right = cam->right;
    const vec3_t up = cam->up;
    const vec3_t forward = cam->forward;
    const vec3_t pos = cam->pos;

    return mat4(right.x, up.x, forward.x,
                    right.y, up.y, forward.y,
                    right.z, up.z, forward.z,
                    -bx::vec3Dot(right.f, pos.f), -bx::vec3Dot(up.f, pos.f), -bx::vec3Dot(forward.f, pos.f));
}

mat4_t tee::camProjMtx(const Camera* cam, float aspectRatio)
{
    float xscale = 1.0f /bx::tan(bx::toRad(cam->fov)*0.5f);
    float yscale = aspectRatio*xscale;
    float zf = cam->ffar;
    float zn = cam->fnear;

    return mat4(xscale, 0, 0, 0,
                   0, yscale, 0, 0,
                   0, 0, zf / (zf - zn), 1.0f,
                   0, 0, zn*zf / (zn - zf), 0);
}

void tee::cam2dInit(Camera2D* cam, float refWidth, float refHeight, DisplayPolicy::Enum policy, float zoom /*= 1.0f*/, 
                        const vec2_t pos /*= vec2_t(0, 0)*/)
{
    cam->refWidth = refWidth;
    cam->refHeight = refHeight;
    cam->zoom = zoom;
    cam->zoomPercentOffset = 0;
    cam->pos = pos;
    cam->policy = policy;
}

void tee::cam2dPan(Camera2D* cam, vec2_t pan)
{
    cam->pos = cam->pos + pan;
}

void tee::cam2dZoom(Camera2D* cam, float zoom)
{
    cam->zoom = zoom;
}

mat4_t tee::cam2dViewMtx(const Camera2D& cam)
{
    return mat4(1.0f, 0, 0,
                    0, 1.0f, 0,
                    0, 0, 1.0f,
                    -cam.pos.x, -cam.pos.y, 0);
}

static vec2_t calcCam2dHalfSize(const Camera2D& cam)
{
    float zoom = cam.getZoom();
    float s = 1.0f / zoom;

    // keep the ratio in scale of 1.0
    float hw, hh;
    float ratio = cam.refWidth / cam.refHeight;
    if (cam.policy == DisplayPolicy::FitToHeight) {
        hw = 0.5f;
        hh = hw / ratio;
    } else if (cam.policy == DisplayPolicy::FitToWidth) {
        hh = 0.5f;
        hw = ratio * hh;
    } else {
        hh = 0.5f;
        hw = 0.5f;
    }
    
    return vec2(hw*s, hh*s);
}

mat4_t tee::cam2dProjMtx(const Camera2D& cam)
{
    mat4_t projMtx;
    vec2_t halfSize = calcCam2dHalfSize(cam);
    bx::mtxOrtho(projMtx.f, -halfSize.x, halfSize.x, -halfSize.y, halfSize.y, 0, 1.0f, 0, false);
    return projMtx;
}

rect_t tee::cam2dGetRect(const Camera2D& cam)
{
    vec2_t halfSize = calcCam2dHalfSize(cam);
    vec2_t pos = cam.pos;
    return rect(-halfSize.x + pos.x, -halfSize.y + pos.y, halfSize.x + pos.x, halfSize.y + pos.y);
}
