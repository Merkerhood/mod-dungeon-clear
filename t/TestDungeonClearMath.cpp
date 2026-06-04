/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"
#include "DungeonClearMath.h"

// Test point directly on the segment (midpoint)
TEST(DungeonClearMathTest, PointOnSegmentMidpoint)
{
    float px = 5.0f;
    float py = 0.0f;
    float ax = 0.0f;
    float ay = 0.0f;
    float bx = 10.0f;
    float by = 0.0f;

    // Midpoint (5,0) on segment (0,0)-(10,0) -> distance should be 0
    EXPECT_NEAR(DungeonClearMath::DistSqToSegment2D(px, py, ax, ay, bx, by), 0.0f, 1e-5f);
}

// Test point on the segment endpoints
TEST(DungeonClearMathTest, PointOnSegmentEndpoints)
{
    float ax = 0.0f;
    float ay = 0.0f;
    float bx = 10.0f;
    float by = 0.0f;

    // At A (0,0) -> distance should be 0
    EXPECT_NEAR(DungeonClearMath::DistSqToSegment2D(0.0f, 0.0f, ax, ay, bx, by), 0.0f, 1e-5f);

    // At B (10,0) -> distance should be 0
    EXPECT_NEAR(DungeonClearMath::DistSqToSegment2D(10.0f, 0.0f, ax, ay, bx, by), 0.0f, 1e-5f);
}

// Test point collinear with segment but outside the endpoints
TEST(DungeonClearMathTest, PointCollinearOutsideSegment)
{
    float ax = 0.0f;
    float ay = 0.0f;
    float bx = 10.0f;
    float by = 0.0f;

    // Before A (-2,0) -> closest point should be A (0,0) -> distance squared should be 4
    EXPECT_NEAR(DungeonClearMath::DistSqToSegment2D(-2.0f, 0.0f, ax, ay, bx, by), 4.0f, 1e-5f);

    // After B (12,0) -> closest point should be B (10,0) -> distance squared should be 4
    EXPECT_NEAR(DungeonClearMath::DistSqToSegment2D(12.0f, 0.0f, ax, ay, bx, by), 4.0f, 1e-5f);
}

// Test point with perpendicular offset from segment
TEST(DungeonClearMathTest, PointWithPerpendicularOffset)
{
    float ax = 0.0f;
    float ay = 0.0f;
    float bx = 10.0f;
    float by = 0.0f;

    // Above midpoint (5,3) -> closest point should be (5,0) -> distance squared should be 9
    EXPECT_NEAR(DungeonClearMath::DistSqToSegment2D(5.0f, 3.0f, ax, ay, bx, by), 9.0f, 1e-5f);

    // Below midpoint (5,-4) -> closest point should be (5,0) -> distance squared should be 16
    EXPECT_NEAR(DungeonClearMath::DistSqToSegment2D(5.0f, -4.0f, ax, ay, bx, by), 16.0f, 1e-5f);
}

// Test zero-length segment (A == B)
TEST(DungeonClearMathTest, ZeroLengthSegment)
{
    float ax = 5.0f;
    float ay = 5.0f;
    float bx = 5.0f;
    float by = 5.0f;

    // Segment is a single point (5,5). Distance to (5,8) should be 3 -> distance squared should be 9
    EXPECT_NEAR(DungeonClearMath::DistSqToSegment2D(5.0f, 8.0f, ax, ay, bx, by), 9.0f, 1e-5f);

    // Distance to (2,5) should be 3 -> distance squared should be 9
    EXPECT_NEAR(DungeonClearMath::DistSqToSegment2D(2.0f, 5.0f, ax, ay, bx, by), 9.0f, 1e-5f);
}

// Test diagonal segment
TEST(DungeonClearMathTest, DiagonalSegment)
{
    // Segment from (0,0) to (6,6)
    float ax = 0.0f;
    float ay = 0.0f;
    float bx = 6.0f;
    float by = 6.0f;

    // Point (0,6) -> projection on line is (3,3) -> distance to (3,3) is sqrt(3^2 + 3^2) = sqrt(18)
    // Distance squared should be 18
    EXPECT_NEAR(DungeonClearMath::DistSqToSegment2D(0.0f, 6.0f, ax, ay, bx, by), 18.0f, 1e-5f);

    // Point (3,3) is on the diagonal -> distance should be 0
    EXPECT_NEAR(DungeonClearMath::DistSqToSegment2D(3.0f, 3.0f, ax, ay, bx, by), 0.0f, 1e-5f);
}
