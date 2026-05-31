#include "engine/subsystemCollection.h"

// SubsystemCollection is currently a typed ownership container. The translation
// unit keeps the boundary visible and gives future subsystem lifecycle hooks a
// stable home without pushing them back into EngineLoop.
