/* Mednafen - Multi-system Emulator
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <math.h>
#include <string.h>

#include "pcfx.h"
#include "soundbox.h"
#include "king.h"
#include "pce_psg/pce_psg.h"

#include "../cdrom/scsicd.h"
#include "../sound/OwlResampler.h"
#include "../clamp.h"
#include "../state_helpers.h"

static const int StepSizes[49] =
{
 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50,
 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157,
 173, 190,  209, 230, 253, 279, 307, 337, 371, 408, 449,
 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552
};

static const int StepIndexDeltas[16] =
{
 -1, -1, -1, -1, 2, 4, 6, 8,
 -1, -1, -1, -1, 2, 4, 6, 8
};

static OwlResampler* FXres = NULL;
static OwlBuffer* FXsbuf[2] = { NULL, NULL };
RavenBuffer* FXCDDABufs[2] = { NULL, NULL };	// Used in the CDROM code

static PCE_PSG *pce_psg = NULL;

static bool SoundEnabled;
static uint32 adpcm_lastts;

struct SoundBox
{
   uint16 ADPCMControl;
   uint8 ADPCMVolume[2][2]; // ADPCMVolume[channel(0 or 1)][left(0) or right(1)]
   uint8 CDDAVolume[2];
   int32 bigdiv;
   int32 smalldiv;

   int64 ResetAntiClick[2];
   double VolumeFiltered[2][2];
   double vf_xv[2][2][1+1], vf_yv[2][2][1+1];

   int32 ADPCMDelta[2];
   int32 ADPCMHaveDelta[2];

   int32 ADPCMPredictor[2];
   int32 StepSizeIndex[2];

   uint32 ADPCMWhichNibble[2];
   uint16 ADPCMHalfWord[2];
   bool ADPCMHaveHalfWord[2];

   int32 ADPCM_last[2][2];
};

static SoundBox sbox;
static double ADPCMVolTable[0x40];

static bool EmulateBuggyCodec;		// If true, emulate the buggy codec/algorithm used by an official PC-FX ADPCM encoder, rather than how the
					// hardware actually works.
static bool ResetAntiClickEnabled;	// = true;


static void RedoVolume(void)
{
 pce_psg->SetVolume(0.681);	//0.227 * 0.50);
 //ADPCMSynth.volume(0.50);
}

bool SoundBox_SetSoundRate(uint32 rate)
{
   SoundEnabled = (bool)rate;

   if(FXres)
   {
      delete FXres;
      FXres = NULL;
   }

   if(rate > 0)
   {
      FXres = new OwlResampler(PCFX_MASTER_CLOCK / 12, rate, MDFN_GetSettingF("pcfx.resamp_rate_error"), 20, MDFN_GetSettingUI("pcfx.resamp_quality"));

      for(unsigned i = 0; i < 2; i++)
         FXres->ResetBufResampState(FXsbuf[i]);
   }

   RedoVolume();

   return(TRUE);
}

int SoundBox_Init(bool arg_EmulateBuggyCodec, bool arg_ResetAntiClickEnabled)
{
   adpcm_lastts = 0;
   SoundEnabled = false;

   EmulateBuggyCodec = arg_EmulateBuggyCodec;
   ResetAntiClickEnabled = arg_ResetAntiClickEnabled;

   for(unsigned i = 0; i < 2; i++)
   {
      FXsbuf[i] = new OwlBuffer();
      FXCDDABufs[i] = new RavenBuffer();
   }

   pce_psg = new PCE_PSG(FXsbuf[0]->Buf(), FXsbuf[1]->Buf(), PCE_PSG::REVISION_HUC6280A);

   memset(&sbox, 0, sizeof(sbox));

   // Build ADPCM volume table, 1.5dB per step, ADPCM volume settings of 0x0 through 0x1B result in silence.
   for(int x = 0; x < 0x40; x++)
   {
      double flub = 1;
      int vti = 0x3F - x;

      if(x) 
         flub /= powf(2, (double)1 / 4 * x);

      if(vti <= 0x1B)
         ADPCMVolTable[vti] = 0;
      else
         ADPCMVolTable[vti] = flub;
   }

   return (1);
}

void SoundBox_Kill(void)
{
   if(pce_psg)
   {
      delete pce_psg;
      pce_psg = NULL;
   }

   for(unsigned i = 0; i < 2; i++)
   {
      if(FXsbuf[i])
      {
         delete FXsbuf[i];
         FXsbuf[i] = NULL;
      }
      if(FXCDDABufs[i])
      {
         delete FXCDDABufs[i];
         FXCDDABufs[i] = NULL;
      }
   }

   if(FXres)
   {
      delete FXres;
      FXres = NULL;
   }
}

/* Macro to access currently selected PSG channel */
void SoundBox_Write(uint32 A, uint16 V, const v810_timestamp_t timestamp)
{
   A &= 0x3F;

   if(A < 0x20)
      pce_psg->Write(timestamp / 3, A >> 1, V);
   else
   {
      switch(A & 0x3F)
      {
         case 0x20:
            SoundBox_ADPCMUpdate(timestamp);
            for(int ch = 0; ch < 2; ch++)
            {
               if(!(sbox.ADPCMControl & (0x10 << ch)) && (V & (0x10 << ch)))
               {
                  if(ResetAntiClickEnabled)
                  {
                     sbox.ResetAntiClick[ch] += (int64)sbox.ADPCMPredictor[ch] << 32;
                     if(sbox.ResetAntiClick[ch] > ((int64)0x3FFF << 32))
                        sbox.ResetAntiClick[ch] = (int64)0x3FFF << 32;
                     if(sbox.ResetAntiClick[ch] < ((int64)-0x4000 << 32))
                        sbox.ResetAntiClick[ch] = (int64)-0x4000 << 32;
                  }

                  sbox.ADPCMPredictor[ch] = 0;
                  sbox.StepSizeIndex[ch] = 0;
               }
            }
            sbox.ADPCMControl = V; 
            break;

         case 0x22:
            SoundBox_ADPCMUpdate(timestamp);
            sbox.ADPCMVolume[0][0] = V & 0x3F; 
            break;

         case 0x24:
            SoundBox_ADPCMUpdate(timestamp);
            sbox.ADPCMVolume[0][1] = V & 0x3F;
            break;

         case 0x26:
            SoundBox_ADPCMUpdate(timestamp);
            sbox.ADPCMVolume[1][0] = V & 0x3F;
            break;

         case 0x28:
            SoundBox_ADPCMUpdate(timestamp);
            sbox.ADPCMVolume[1][1] = V & 0x3F; 
            break;

         case 0x2A:
            sbox.CDDAVolume[0] = V & 0x3F;
            SCSICD_SetCDDAVolume(0.50f * sbox.CDDAVolume[0] / 63, 0.50f * sbox.CDDAVolume[1] / 63);
            break;

         case 0x2C:
            sbox.CDDAVolume[1] = V & 0x3F;
            SCSICD_SetCDDAVolume(0.50f * sbox.CDDAVolume[0] / 63, 0.50f * sbox.CDDAVolume[1] / 63);
            break;
      }
   }
}

static uint32 KINGADPCMControl;

void SoundBox_SetKINGADPCMControl(uint32 value)
{
   KINGADPCMControl = value;
}

/* Digital filter designed by mkfilter/mkshape/gencode   A.J. Fisher
   Command line: /www/usr/fisher/helpers/mkfilter -Bu -Lp -o 1 -a 1.5888889125e-04 0.0000000000e+00 -l */
static void DoVolumeFilter(int ch, int lr)
{
   sbox.vf_xv[ch][lr][0] = sbox.vf_xv[ch][lr][1]; 
   sbox.vf_xv[ch][lr][1] = (double)ADPCMVolTable[sbox.ADPCMVolume[ch][lr]] / 2.004348738e+03;

   sbox.vf_yv[ch][lr][0] = sbox.vf_yv[ch][lr][1]; 
   sbox.vf_yv[ch][lr][1] = (sbox.vf_xv[ch][lr][0] + sbox.vf_xv[ch][lr][1]) + (  0.9990021696 * sbox.vf_yv[ch][lr][0]);
   sbox.VolumeFiltered[ch][lr] = sbox.vf_yv[ch][lr][1];
}

static const int16 ADPCM_PhaseFilter[8][7] =
{
 /*   0 */ {    40,   283,   654,   683,   331,    56,     1 }, //  2048
 /*   1 */ {    28,   238,   618,   706,   381,    75,     2 }, //  2048
 /*   2 */ {    19,   197,   577,   720,   432,    99,     4 }, //  2048
 /*   3 */ {    12,   160,   532,   726,   483,   128,     7 }, //  2048
 /*   4 */ {     7,   128,   483,   726,   532,   160,    12 }, //  2048
 /*   5 */ {     4,    99,   432,   720,   577,   197,    19 }, //  2048
 /*   6 */ {     2,    75,   381,   706,   618,   238,    28 }, //  2048
 /*   7 */ {     1,    56,   331,   683,   654,   283,    40 }, //  2048
};

v810_timestamp_t SoundBox_ADPCMUpdate(const v810_timestamp_t timestamp)
{
   int32 run_time = timestamp - adpcm_lastts;

   adpcm_lastts = timestamp;

   sbox.bigdiv -= run_time * 2;

   while(sbox.bigdiv <= 0)
   {
      sbox.smalldiv--;
      while(sbox.smalldiv <= 0)
      {
         sbox.smalldiv += 1 << ((KINGADPCMControl >> 2) & 0x3);
         for(int ch = 0; ch < 2; ch++)
         {
            // Keep playing our last halfword fetched even if KING ADPCM is disabled
            if(sbox.ADPCMHaveHalfWord[ch] || KINGADPCMControl & (1 << ch)) 
            {
               if(!sbox.ADPCMWhichNibble[ch])
               {
                  sbox.ADPCMHalfWord[ch] = KING_GetADPCMHalfWord(ch);
                  sbox.ADPCMHaveHalfWord[ch] = TRUE;
               }

               // If the channel's reset bit is set, don't update its ADPCM state.
               if(sbox.ADPCMControl & (0x10 << ch))
               {
                  sbox.ADPCMDelta[ch] = 0;
               }
               else
               {
                  uint8 nibble = (sbox.ADPCMHalfWord[ch] >> (sbox.ADPCMWhichNibble[ch])) & 0xF;
                  int32 BaseStepSize = StepSizes[sbox.StepSizeIndex[ch]];

                  if(EmulateBuggyCodec)
                  {
                     if(BaseStepSize == 1552)
                        BaseStepSize = 1522;

                     sbox.ADPCMDelta[ch] = BaseStepSize * ((nibble & 0x7) + 1) * 2;
                  }
                  else
                     sbox.ADPCMDelta[ch] = BaseStepSize * ((nibble & 0x7) + 1);

                  // Linear interpolation turned on?
                  if(sbox.ADPCMControl & (0x4 << ch))
                     sbox.ADPCMDelta[ch] >>= (KINGADPCMControl >> 2) & 0x3;

                  if(nibble & 0x8)
                     sbox.ADPCMDelta[ch] = -sbox.ADPCMDelta[ch];

                  sbox.StepSizeIndex[ch] += StepIndexDeltas[nibble];

                  if(sbox.StepSizeIndex[ch] < 0)
                     sbox.StepSizeIndex[ch] = 0;

                  if(sbox.StepSizeIndex[ch] > 48)
                     sbox.StepSizeIndex[ch] = 48;
               }
               sbox.ADPCMHaveDelta[ch] = 1;

               // Linear interpolation turned on?
               if(sbox.ADPCMControl & (0x4 << ch))
                  sbox.ADPCMHaveDelta[ch] = 1 << ((KINGADPCMControl >> 2) & 0x3);

               sbox.ADPCMWhichNibble[ch] = (sbox.ADPCMWhichNibble[ch] + 4) & 0xF;

               if(!sbox.ADPCMWhichNibble[ch])
                  sbox.ADPCMHaveHalfWord[ch] = FALSE;
            }
         } // for(int ch...)
      } // while(sbox.smalldiv <= 0)

      const uint32 synthtime42 = (timestamp << 1) + sbox.bigdiv;
      const uint32 synthtime14 = synthtime42 / 3;
      const uint32 synthtime = synthtime14 >> 3;
      const unsigned synthtime_phase = synthtime14 & 7;

      for(int ch = 0; ch < 2; ch++)
      {
         if(sbox.ADPCMHaveDelta[ch]) 
         {
            sbox.ADPCMPredictor[ch] += sbox.ADPCMDelta[ch];

            sbox.ADPCMHaveDelta[ch]--;

            if(sbox.ADPCMPredictor[ch] > 0x3FFF) { sbox.ADPCMPredictor[ch] = 0x3FFF; }
            if(sbox.ADPCMPredictor[ch] < -0x4000) { sbox.ADPCMPredictor[ch] = -0x4000;  }
         }
         else
         {

         }

         if(SoundEnabled)
         {
            int32 samp[2];

            if(EmulateBuggyCodec)
            {
               samp[0] = (int32)(((sbox.ADPCMPredictor[ch] >> 1) + (sbox.ResetAntiClick[ch] >> 33)) * sbox.VolumeFiltered[ch][0]);
               samp[1] = (int32)(((sbox.ADPCMPredictor[ch] >> 1) + (sbox.ResetAntiClick[ch] >> 33)) * sbox.VolumeFiltered[ch][1]);
            }
            else
            {
               samp[0] = (int32)((sbox.ADPCMPredictor[ch] + (sbox.ResetAntiClick[ch] >> 32)) * sbox.VolumeFiltered[ch][0]);
               samp[1] = (int32)((sbox.ADPCMPredictor[ch] + (sbox.ResetAntiClick[ch] >> 32)) * sbox.VolumeFiltered[ch][1]);
            }
            for(unsigned y = 0; y < 2; y++)
            {
               const int32 delta = samp[y] - sbox.ADPCM_last[ch][y];
               int32* tb = FXsbuf[y]->Buf() + (synthtime & 0xFFFF);
               const int16* coeffs = ADPCM_PhaseFilter[synthtime_phase];

               for(unsigned c = 0; c < 7; c++)
               {
                  int32 tmp = delta * coeffs[c];

                  tb[c] += tmp;
               }
            }

            sbox.ADPCM_last[ch][0] = samp[0];
            sbox.ADPCM_last[ch][1] = samp[1];
         }
      }

      for(int ch = 0; ch < 2; ch++)
      {
         sbox.ResetAntiClick[ch] -= sbox.ResetAntiClick[ch] >> 8;
         //if(ch)
         // MDFN_DispMessage("%d", (int)(sbox.ResetAntiClick[ch] >> 32));
      }

      for(int ch = 0; ch < 2; ch++)
         for(int lr = 0; lr < 2; lr++)
         {
            DoVolumeFilter(ch, lr);
         }
      sbox.bigdiv += 1365 * 2 / 2;
   }

   return(timestamp + (sbox.bigdiv + 1) / 2);
}

int32 SoundBox_Flush(const v810_timestamp_t end_timestamp, v810_timestamp_t* new_base_timestamp, int16 *SoundBuf, const int32 MaxSoundFrames)
{
   const uint32 end_timestamp_div3 = end_timestamp / 3;
   const uint32 end_timestamp_div12 = end_timestamp / 12;
   const uint32 end_timestamp_mod12 = end_timestamp % 12;
   const unsigned rsc = std::min<unsigned>(65536, end_timestamp_div12);
   int32 FrameCount = 0;

   *new_base_timestamp = end_timestamp_mod12;

   pce_psg->Update(end_timestamp_div3);

   for(unsigned y = 0; y < 2; y++)
   {
      if(SoundEnabled && FXres)
      {
         FXsbuf[y]->Integrate(rsc, 0, 0, FXCDDABufs[y]);
         FrameCount = FXres->Resample(FXsbuf[y], rsc, SoundBuf + y, MaxSoundFrames);
      }
      else
         FXsbuf[y]->ResampleSkipped(rsc);

      FXCDDABufs[y]->Finish(rsc);
   }

   return(FrameCount);
}

void SoundBox_ResetTS(const v810_timestamp_t ts_base)
{
   pce_psg->ResetTS(ts_base / 3);
   adpcm_lastts = ts_base;
}

void SoundBox_Reset(const v810_timestamp_t timestamp)
{
   SoundBox_ADPCMUpdate(timestamp);
   pce_psg->Power(timestamp / 3);

   sbox.ADPCMControl = 0;

   memset(&sbox.vf_xv, 0, sizeof(sbox.vf_xv));
   memset(&sbox.vf_yv, 0, sizeof(sbox.vf_yv));

   for(int lr = 0; lr < 2; lr++)
   {
      for(int ch = 0; ch < 2; ch++)
      {
         sbox.ADPCMVolume[ch][lr] = 0;
         sbox.VolumeFiltered[ch][lr] = 0;
      }

      sbox.CDDAVolume[lr] = 0;
   }

   for(int ch = 0; ch < 2; ch++)
   {
      sbox.ADPCMPredictor[ch] = 0;
      sbox.StepSizeIndex[ch] = 0;
   }

   memset(sbox.ADPCMWhichNibble, 0, sizeof(sbox.ADPCMWhichNibble));
   memset(sbox.ADPCMHalfWord, 0, sizeof(sbox.ADPCMHalfWord));
   memset(sbox.ADPCMHaveHalfWord, 0, sizeof(sbox.ADPCMHaveHalfWord));

   SCSICD_SetCDDAVolume(0.50f * sbox.CDDAVolume[0] / 63, 0.50f * sbox.CDDAVolume[1] / 63);

   sbox.bigdiv = 2;	// TODO: KING->SBOX ADPCM Synch //(1365 - 85 * 4) * 2; //1365 * 2 / 2;
   sbox.smalldiv = 0;
}

int SoundBox_StateAction(StateMem *sm, int load, int data_only)
{
   int ret = 1;

   ret &= pce_psg->StateAction(sm, load, data_only);

   SFORMAT SoundBox_StateRegs[] =
   {
      SFVARN(sbox.ADPCMControl, "ADPCMControl"),
      SFVARN(sbox.ADPCMVolume, "ADPCMVolume"),
      SFVARN(sbox.CDDAVolume, "CDDAVolume"),
      SFVARN(sbox.bigdiv, "bigdiv"),
      SFVARN(sbox.smalldiv, "smalldiv"),

      SFVARN(sbox.ResetAntiClick, "ResetAntiClick"),
      SFVARN(sbox.VolumeFiltered, "VolumeFiltered"),
      SFVARN(sbox.vf_xv, "vf_xv"),
      SFVARN(sbox.vf_yv, "vf_yv"),

      SFVARN(sbox.ADPCMDelta, "ADPCMDelta"),
      SFVARN(sbox.ADPCMHaveDelta, "ADPCMHaveDelta"),

      SFVARN(sbox.ADPCMPredictor, "ADPCMPredictor"),
      SFVARN(sbox.StepSizeIndex, "ADPCMStepSizeIndex"),

      SFVARN(sbox.ADPCMWhichNibble, "ADPCMWNibble"),
      SFVARN(sbox.ADPCMHalfWord, "ADPCMHalfWord"),
      SFVARN(sbox.ADPCMHaveHalfWord, "ADPCMHHW"),

      SFEND
   };

   ret &= MDFNSS_StateAction(sm, load, data_only, SoundBox_StateRegs, "SBOX", false);

   if(load)
   {
      clamp(&sbox.bigdiv, 1, 1365);
      clamp(&sbox.smalldiv, 1, 8);

      for(int ch = 0; ch < 2; ch++)
      {
         clamp(&sbox.ADPCMPredictor[ch], -0x4000, 0x3FFF);
         clamp(&sbox.ResetAntiClick[ch], -((int64)0x4000 << 32), (int64)0x3FFF << 32);

         if(!ResetAntiClickEnabled)
            sbox.ResetAntiClick[ch] = 0;

         clamp(&sbox.StepSizeIndex[ch], 0, 48);

         for(int lr = 0; lr < 2; lr++)
         {
            sbox.ADPCMVolume[ch][lr] &= 0x3F;
         }
      }
      SCSICD_SetCDDAVolume(0.50f * sbox.CDDAVolume[0] / 63, 0.50f * sbox.CDDAVolume[1] / 63);
   }
   return(ret); 
}
