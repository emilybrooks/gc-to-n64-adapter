#include "Nintendo.h"

#define SWITCH_PIN 2
#define GC_PIN 3
#define N64_PIN 4

// how far the c stick has to move away from the origin
// to trigger a c button input
#define CSTICK_THRESHOLD 40

// when the deadzone is enabled,
// how many analog stick values from the origin to ignore
#define DEADZONE_RADIUS 12

CGamecubeController gamecube_controller(GC_PIN);
CN64Console n64_console(N64_PIN);

void setup()
{
    Serial.begin(115200);
    pinMode(SWITCH_PIN, INPUT_PULLUP);
}

void loop()
{
    if (!gamecube_controller.read()) return;

    Gamecube_Report_t input = gamecube_controller.getReport();

    /*
    this requires a special fork of nintendospy
    https://github.com/ClydePowers/NintendoSpy-1/releases

    nintendospy normally expects individual bits, but calling Serial.write()
    64 times is too slow, preventing the arduino from responding to the n64
    fast enough.

    this fork allows us to send all 8 bytes in one Serial.write()
    */

    //send data to input viewer
    Serial.write(input.raw8, sizeof(input.raw8));
    Serial.write('\n');
    
    N64_Data_t data = defaultN64Data;

    data.report.a = input.a;
    data.report.b = input.b;
    data.report.z = input.l;
    data.report.r = input.r;
    data.report.start = input.start;
    data.report.dup = input.dup;
    data.report.ddown = input.ddown;
    data.report.dleft = input.dleft;
    data.report.dright = input.dright;

    if (input.cyAxis > 128 + CSTICK_THRESHOLD) data.report.cup |= true;

    if (input.cyAxis < 128 - CSTICK_THRESHOLD) data.report.cdown |= true;
    data.report.cdown |= input.z;

    if (input.cxAxis < 128 - CSTICK_THRESHOLD) data.report.cleft |= true;
    data.report.cleft |= input.y;

    if (input.cxAxis > 128 + CSTICK_THRESHOLD) data.report.cright |= true;
    data.report.cright |= input.x;

    convert_analog_stick(&input, &data.report);

    if (digitalRead(SWITCH_PIN) == LOW)
    {
        convert_to_deadzone(&data.report, DEADZONE_RADIUS);
    }

    n64_console.write(data);
}

//------------------------------------------------------------------------------
// If the analog coordinate is below the deadzone radius, it will be set to 0
// otherwise the analog coordinate range is shrunken
// in order to fit into the space between the deadzone and the maximum
//
// n64:      n64 console report to write to
// deadzone: how many analog stick values from the origin to ignore
//------------------------------------------------------------------------------
void convert_to_deadzone(N64_Report_t* n64, uint8_t deadzone)
{
    const uint8_t N64_RANGE = 80;
    const uint8_t NEW_RANGE = N64_RANGE - deadzone;
    boolean x_negative = false;
    boolean y_negative = false;
    double new_x = 0;
    double new_y = 0;
    //TODO: could this be done with integer division? would that be more accurate?

    //convert to quadrant 1 to make life easier
    if (n64->xAxis < 0) x_negative = true;
    n64->xAxis = abs(n64->xAxis) ;

    if (n64->yAxis < 0) y_negative = true;
    n64->yAxis = abs(n64->yAxis) ;

    if (n64->xAxis < deadzone)
    {
        n64->xAxis = 0;
    }
    else
    {
        new_x = n64->xAxis - deadzone;
        new_x = (new_x / NEW_RANGE) * N64_RANGE;
    }

    if (n64->yAxis < deadzone)
    {
        n64->yAxis = 0;
    }
    else
    {
        new_y = n64->yAxis - deadzone;
        new_y = (new_y / NEW_RANGE) * N64_RANGE;
    }

    n64->xAxis = (char)round(new_x);
    n64->yAxis = (char)round(new_y);

    //restore the correct direction
    if (x_negative) n64->xAxis = -n64->xAxis;
    if (y_negative) n64->yAxis = -n64->yAxis;
}

/*
Originally part of "oot-vc-adapter" by NAM

The MIT License (MIT)

Copyright (c) 2019 NAM

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

//------------------------------------------------------------------------------
// Converts Gamecube analog stick coordinates into N64 analog stick coordinates
//
// gc:  gamecube controller report to read the analog coordinates from
// n64: n64 console report to write the converted coordinates to
//------------------------------------------------------------------------------
void convert_analog_stick(Gamecube_Report_t* gc, N64_Report_t* n64)
{
    uint8_t new_x = gc->xAxis;
    uint8_t new_y = gc->yAxis;

// because of symmetry, this function expects coordinates such that
// 0 <= y <= x <= 127
// our coordinates need to be prepped for this
// gamecube coordinates come in an unsigned byte (0 to 255)
// and first need to be converted to a signed byte range (-128 to 127)
// by subtracting 128
// then keep track of the negative for later, and turn it into a positive
    boolean x_negative = false;
    boolean y_negative = false;

    if (new_x < 128) x_negative = true;
    new_x = abs(new_x - 128);
    if (new_y < 128) y_negative = true;
    new_y = abs(new_y - 128);

    //swapping the coordinates might be required to keep y <= x
    boolean swap = false;
    if (new_y > new_x)
    {
        swap = true;
        int8_t temp = new_x;
        new_x = new_y;
        new_y = temp;
    }
    
/*
Achievable range on a gamecube controller is 75 in corners and 105 straight
N64 ranges from 70 in the corners to 80 straight. The shape is different.
To maximize precision while allowing full range, we need to scale:
  - Straight directions to 80/105
  - Corner directions to 70/75
Because this stretching effect warps the shape of the controller,
we'd like to minimize our warping in the center and scale it up near edge.
 
First we try to find the intersection point with the edge of the range.
  distance = (5x+2y) / 525
  closeness_to_corner = 7y / 5x+2y
These range from 0 to 1 and derive from the formula for line intersection: 
  https://gamedev.stackexchange.com/questions/44720/
Our conversion formula becomes:
  extra_corner_scaling = 70/75-80/105
  scale = distance^3 * closeness_to_corner * extra_corner_scaling + 80/105
  return x * scale, y * scale
The cubing of distance means we warp very little in the center.

We implement the below formula in uint32 integer math:
  ((5x + 2y) / 525)^2 * (7y / 525) * (70/75-80/105) + 80/105
Notice that the multiplication cancels out one factor of 5x+2y
*/
    uint32_t scale = 5L * new_x + 2L * new_y;
    if (scale > 525)
    {
        // Multiply by 16 here to reduce precision loss from dividing by scale
        scale = 16UL * 525 * 525 * 525 / scale; // clamp distance to 1.0
    }
    else
    {
        // (5x + 2y)^2, leaving 525^2 to divide later.
        scale = 16 * scale * scale; 
    }

    scale *= new_y; // * y, leaving another 525 to divide later.

    // Now we need to divide by 525^3 and multiply by: 7 * (70/75-80/105) = 1.2
    // And we divide by 2**24 at the end.
    // So our final multiplication factor is 525^3 / 1.2 / 2 / 2^24 ~= 16 / 115
    scale = scale / 115; // we already multiplied by an extra *16 above

    // constants chosen so rounding errors don't affect the end result
    scale += 12782650; // ~= 80/105 * 2^24

    // add a bit less than 2^24 so we round up by truncating.
    // n-0.5 < box[2n]   <= n
    // n     < box[2n+1] <= n+0.5
    new_x = (new_x * scale + 16774000) >> 24;
    new_y = (new_y * scale + 16774000) >> 24;

    //move to a signed byte
    n64->xAxis = new_x;
    n64->yAxis = new_y;
    
    //restore the proper direction
    //swapping first is important, otherwise the negatives won't match
    if (swap)
    {
        int8_t temp = n64->xAxis;
        n64->xAxis = n64->yAxis;
        n64->yAxis = temp;
    }
    if (x_negative) n64->xAxis = -n64->xAxis;
    if (y_negative) n64->yAxis = -n64->yAxis;
}
