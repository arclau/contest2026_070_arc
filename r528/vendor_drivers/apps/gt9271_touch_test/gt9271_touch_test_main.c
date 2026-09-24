/****************************************************************************
 * apps/examples/gt9271_touch_test/gt9271_touch_test_main.c
 *
 * GT9271 LCD IIC Touchscreen Test Application for R528 Platform
 * Generic test application that works with any touch controller
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <nuttx/input/touchscreen.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define GT9271_TOUCHSCREEN_DEVPATH "/dev/input0"
/* GT9271 驱动将原始坐标缩放至 LCD 像素分辨率 (1200x1920, 芯片原生方向)。
 * 横屏显示 (1920x1200, CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT) 下
 * 触摸轴与屏幕显示轴会旋转映射，此处百分比按驱动输出坐标计算。 */
#define GT9271_LCD_WIDTH           1200
#define GT9271_LCD_HEIGHT          1920
#define IOCTL_CMD 0x0011
/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void print_gt9271_touch_point(const struct touch_point_s *point)
{
  printf("GT9271 Touch Point ID %d:\n", point->id);
  printf("  Position: (%d, %d) - %.1f%%, %.1f%%\n",
         point->x, point->y,
         (float)point->x * 100.0f / GT9271_LCD_WIDTH,
         (float)point->y * 100.0f / GT9271_LCD_HEIGHT);
  printf("  Size: %dx%d pixels\n", point->w, point->h);
  printf("  Pressure: %d\n", point->pressure);
  printf("  Flags: 0x%02X", point->flags);

  if (point->flags & TOUCH_DOWN)
    printf(" DOWN");
  if (point->flags & TOUCH_MOVE)
    printf(" MOVE");
  if (point->flags & TOUCH_UP)
    printf(" UP");
  if (point->flags & TOUCH_ID_VALID)
    printf(" ID_VALID");
  if (point->flags & TOUCH_POS_VALID)
    printf(" POS_VALID");
  if (point->flags & TOUCH_PRESSURE_VALID)
    printf(" PRESSURE_VALID");
  if (point->flags & TOUCH_SIZE_VALID)
    printf(" SIZE_VALID");

  printf("\n");

  if (point->flags & TOUCH_GESTURE_VALID)
    {
      printf("  Gesture: ");
      switch (point->gesture)
        {
          case TOUCH_SINGLE_CLICK:
            printf("Single Click\n");
            break;
          case TOUCH_DOUBLE_CLICK:
            printf("Double Click\n");
            break;
          case TOUCH_SLIDE_UP:
            printf("Slide Up\n");
            break;
          case TOUCH_SLIDE_DOWN:
            printf("Slide Down\n");
            break;
          case TOUCH_SLIDE_LEFT:
            printf("Slide Left\n");
            break;
          case TOUCH_SLIDE_RIGHT:
            printf("Slide Right\n");
            break;
          case TOUCH_PALM:
            printf("Palm\n");
            break;
          default:
            printf("Unknown (%d)\n", point->gesture);
            break;
        }
    }

  printf("  Timestamp: %llu μs\n", (unsigned long long)point->timestamp);
  printf("\n");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  int fd;
  ssize_t nbytes;
  struct touch_sample_s *sample;
  int max_samples = 100;
  int sample_count = 0;

  printf("GT9271 LCD IIC Touchscreen Test Application\n");
  printf("==============================================\n");
  printf("LCD Size: %dx%d pixels\n", GT9271_LCD_WIDTH, GT9271_LCD_HEIGHT);
  printf("Device: %s\n", GT9271_TOUCHSCREEN_DEVPATH);
  printf("Auto-detects touch controller type\n");
  printf("Press Ctrl+C to exit\n\n");

  /* Parse command line arguments */

  if (argc > 1)
    {
      max_samples = atoi(argv[1]);
      if (max_samples <= 0)
        {
          max_samples = 100;
        }
    }

  printf("Will read %d samples (or until Ctrl+C)\n\n", max_samples);

  /* Open the GT9271 touchscreen device */

  fd = open(GT9271_TOUCHSCREEN_DEVPATH, O_RDONLY);
  if (fd < 0)
    {
      printf("ERROR: Failed to open GT9271 touchscreen %s: %s\n",
             GT9271_TOUCHSCREEN_DEVPATH, strerror(errno));
      printf("Make sure CONFIG_GT9271_IIC_TOUCH is enabled\n");
      printf("The driver will auto-detect the touch controller type\n");
      return EXIT_FAILURE;
    }

  printf("Successfully opened GT9271 IIC touchscreen device\n");
  printf("Touch controller auto-detection completed\n");
  printf("Waiting for touch events on GT9271 LCD...\n\n");

  /* Allocate buffer for touch sample (assuming max 5 touch points) */

  sample = (struct touch_sample_s *)malloc(SIZEOF_TOUCH_SAMPLE_S(5));
  if (!sample)
    {
      printf("ERROR: Failed to allocate sample buffer\n");
      close(fd);
      return EXIT_FAILURE;
    }

  /* Read touch samples from GT9271 */
  while (1)
    {
      /* Read a touch sample */


      nbytes = read(fd, sample, SIZEOF_TOUCH_SAMPLE_S(5));

      if (nbytes < 0)
        {
          if (errno == EINTR)
            {
              printf("Interrupted by signal\n");
              break;
            }

          printf("ERROR: read() failed: %s\n", strerror(errno));
          break;
        }

      if (nbytes == 0)
        {
          printf("No data available from GT9271 touchscreen\n");
          usleep(100000); /* 100ms */
          continue;
        }

      /* Validate the sample size */

      if (nbytes < sizeof(struct touch_sample_s))
        {
          printf("ERROR: Incomplete sample read: %zd bytes\n", nbytes);
          continue;
        }

      /* Print sample information */

      printf("=== GT9271 Touch Sample %d ===\n", ++sample_count);
      printf("Number of touch points: %d\n", sample->npoints);

      if (sample->npoints > 0)
        {
          for (int i = 0; i < sample->npoints && i < 5; i++)
            {
              printf("\n");
              print_gt9271_touch_point(&sample->point[i]);
            }
        }

      printf("--------------------------------------------\n\n");

      /* Small delay to prevent flooding the output */

      usleep(50000); /* 50ms */
    }

  printf("GT9271 touchscreen test completed\n");
  printf("Total samples read: %d\n", sample_count);

  /* Clean up */

  free(sample);
  close(fd);

  return EXIT_SUCCESS;
}
