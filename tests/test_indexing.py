import drjit as dr
import pytest

@pytest.test_arrays('shape=(3), -bool')
def test01_index_static(t):
    v = t(1, 2, 3)
    assert v.x == 1 and v.y == 2 and v.z == 3
    v.x, v.y, v.z = 4, 5, 6
    assert v.x == 4 and v.y == 5 and v.z == 6
    assert v[0] == 4 and v[1] == 5 and v[2] == 6
    assert v[-1] == 6 and v[-2] == 5 and v[-3] == 4
    assert len(v) == 3

    with pytest.raises(RuntimeError, match="does not have a"):
        v.w = 4
    with pytest.raises(RuntimeError, match="does not have a"):
        v.imag = 4
    with pytest.raises(IndexError, match=r"entry 3 is out of bounds \(the array is of size 3\)."):
        v[3]
    with pytest.raises(IndexError, match=r"entry -1 is out of bounds \(the array is of size 3\)."):
        v[-4]

    assert v.shape == (3,)

@pytest.test_arrays('shape=(*), -bool')
def test01_index_dynamic(t):
    v = t(1, 2, 3)
    assert v[0] == 1 and v[1] == 2 and v[2] == 3
    v[0], v[1], v[2] = 4, 5, 6
    assert v[0] == 4 and v[1] == 5 and v[2] == 6
    assert v[-1] == 6 and v[-2] == 5 and v[-3] == 4
    assert len(v) == 3

    with pytest.raises(RuntimeError, match="does not have a"):
        v.x = 4
    with pytest.raises(RuntimeError, match="does not have a"):
        v.imag = 4
    with pytest.raises(IndexError, match=r"entry 3 is out of bounds \(the array is of size 3\)."):
        v[3]
    with pytest.raises(IndexError, match=r"entry -1 is out of bounds \(the array is of size 3\)."):
        v[-4]

    assert v.shape == (3,)
    assert t(1).shape == (1,)