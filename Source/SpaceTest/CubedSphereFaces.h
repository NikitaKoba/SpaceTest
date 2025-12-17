#pragma once

#include "CoreMinimal.h"

struct FCubedSphereFace
{
	FVector Normal;
	FVector Right;
	FVector Up;
};

static const FCubedSphereFace Faces[6] = {
	{ FVector(1, 0, 0),  FVector(0, 1, 0),  FVector(0, 0, 1) },  // +X
	{ FVector(-1, 0, 0), FVector(0, -1, 0), FVector(0, 0, 1) }, // -X
	{ FVector(0, 1, 0),  FVector(1, 0, 0),  FVector(0, 0, -1) }, // +Y
	{ FVector(0, -1, 0), FVector(1, 0, 0),  FVector(0, 0, 1) },  // -Y
	{ FVector(0, 0, 1),  FVector(1, 0, 0),  FVector(0, 1, 0) },  // +Z
	{ FVector(0, 0, -1), FVector(1, 0, 0),  FVector(0, -1, 0) }  // -Z
};
