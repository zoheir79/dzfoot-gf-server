"""
Coordinate system conversion utilities.
3DS Max .ase uses Z-up, glTF uses Y-up.
Conversion: rotate -90 degrees around X axis.
"""
import math
from typing import Tuple, List

# Quaternion for -90° rotation around X axis (Z-up to Y-up)
_HALF_SQRT2 = math.sqrt(2.0) / 2.0
Q_ZUP_TO_YUP = (-_HALF_SQRT2, 0.0, 0.0, _HALF_SQRT2)  # (x, y, z, w)
Q_YUP_TO_ZUP = (_HALF_SQRT2, 0.0, 0.0, _HALF_SQRT2)   # conjugate


def quat_multiply(a: Tuple[float, float, float, float],
                  b: Tuple[float, float, float, float]) -> Tuple[float, float, float, float]:
    """Multiply two quaternions (x, y, z, w)."""
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    )


def quat_conjugate(q: Tuple[float, float, float, float]) -> Tuple[float, float, float, float]:
    """Conjugate of a quaternion."""
    return (-q[0], -q[1], -q[2], q[3])


def vec3_zup_to_yup(v: Tuple[float, float, float]) -> Tuple[float, float, float]:
    """Convert a 3D vector from Z-up to Y-up: (x, y, z) -> (x, z, -y)."""
    return (v[0], v[2], -v[1])


def quat_zup_to_yup(q: Tuple[float, float, float, float]) -> Tuple[float, float, float, float]:
    """Convert a quaternion from Z-up to Y-up coordinate system."""
    # q_yup = Q_ZUP_TO_YUP * q * conj(Q_ZUP_TO_YUP)
    temp = quat_multiply(Q_ZUP_TO_YUP, q)
    return quat_multiply(temp, Q_YUP_TO_ZUP)


def axis_zup_to_yup(ax: float, ay: float, az: float) -> Tuple[float, float, float]:
    """Convert an axis vector from Z-up to Y-up."""
    return (ax, az, -ay)


def normalize_quat(q: Tuple[float, float, float, float]) -> Tuple[float, float, float, float]:
    """Normalize a quaternion."""
    x, y, z, w = q
    length = math.sqrt(x * x + y * y + z * z + w * w)
    if length < 1e-12:
        return (0.0, 0.0, 0.0, 1.0)
    return (x / length, y / length, z / length, w / length)
