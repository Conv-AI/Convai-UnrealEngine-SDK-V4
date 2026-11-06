// Copyright Convai Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

namespace ConvaiAvatarDioramaPrivate
{
/** One static mesh component as the triangle total counts it: its mesh's LOD0 triangles, drawn Instances times. */
struct FMeshDraw
{
    int32 Triangles = 0;
    int32 Instances = 0;
};

/** LOD0 triangles over every static mesh draw and skeletal mesh, capped at MAX_int32 so a total too big to hold still breaks the limit. */
int32 TotalTriangles(TConstArrayView<FMeshDraw> StaticDraws, TConstArrayView<int32> SkeletalFaces);
}
