#pragma once



#include "UnrealEnginePython.h"

#if WITH_EDITOR
PyObject *py_ue_create_landscape_info(ue_PyUObject *self, PyObject *);
PyObject *py_ue_get_landscape_info(ue_PyUObject *self, PyObject *);
PyObject *py_ue_landscape_import(ue_PyUObject *self, PyObject *);
#endif