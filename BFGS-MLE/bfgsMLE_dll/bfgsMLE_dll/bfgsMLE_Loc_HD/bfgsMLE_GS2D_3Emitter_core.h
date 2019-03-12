/*
This program is free software: you can redistribute it and/or modify
it under the terms of the GNU LESSER GENERAL PUBLIC LICENSE as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU LESSER GENERAL PUBLIC LICENSE for more details.

You should have received a copy of the GNU LESSER GENERAL PUBLIC LICENSE
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#include "bfgsMLE_core.h"

#include "WLEParaEstimation_Parameters.h"


// MLE fitting iteration number 
#define FittedFluoNum_2D_3E			3 // constant, don't modify

#define IterateNum_2D_3E			16  // total iteration number, fixed iteration number
#define IterateNum_2D_3E_bs			11 // bisection iteration to find best walk length


// number of fitting parameter
#define FitParaNum_2D_3E			11

// fitting parameters ID
#define Fit2D_3E_Peak0				0
#define Fit2D_3E_XPos0				1
#define Fit2D_3E_YPos0				2

#define Fit2D_3E_Peak1				3
#define Fit2D_3E_XPos1				4
#define Fit2D_3E_YPos1				5

#define Fit2D_3E_Peak2				6
#define Fit2D_3E_XPos2				7
#define Fit2D_3E_YPos2				8

#define Fit2D_3E_Sigm				9
#define Fit2D_3E_Bakg				10



template <int ROISize, int ROIPixelNum, int FitParaNum, int IterateNum, int IterateNum_bs>
__device__ void MLELocalization_GS2D_3E(float ImageROI[][ROIPixelNum], float Ininf[][ThreadsPerBlock], float grad[][ThreadsPerBlock], float d0[][ThreadsPerBlock], float D0[], float* WLE_Weight, int tid);


template <int ROISize, int ROIPixelNum, int FitParaNum>
__device__ void poissonfGradient_GS2D_3E(float ImageROI[][ROIPixelNum], float Ininf[][ThreadsPerBlock], float grad[][ThreadsPerBlock], float* WLE_Weight, int tid);


template <int ROISize, int ROIPixelNum, int FitParaNum>
__device__ float MLEFit_TargetF_GS2D_3E(float ImageROI[][ROIPixelNum], float Ininf[], float* WLE_Weight, int tid);


template <int ROISize, int ROIPixelNum, int FitParaNum>
__device__ void PreEstimation_GS2D_3E(float ImageROI[][ROIPixelNum], float Ininf[][ThreadsPerBlock], int tid);



// algorithm core codes
template <int ROISize, int FitParaNum>
__global__ void bfgsMLELoc_Gauss2D_3E(float *d_LocArry, unsigned short *d_ImageROI, float *d_WLEPara, int MultiFitFluoNum, int * d_MultiFitFluoPos, float Offset, float kadc, float QE)
{

	enum {
		ROIPixelNum = (ROISize*ROISize),
		ROIWholeSize = (ROISize*(ROISize + 1)),


		ROISize_Half = (int)(ROISize / 2),

		LoadLoopNum = ((ROIPixelNum + ThreadsPerBlock - 1) / ThreadsPerBlock)
	};

#define XYPosVaryBound		1.5f

	float XYLBound = ROISize / 2.0f - XYPosVaryBound; // 1.0f
	float XYUBound = ROISize / 2.0f + XYPosVaryBound;

	float Simga_Max_Th = ROISize / 1.9f / 2.35f;

	//
	__shared__ float ImageROI[ThreadsPerBlock][ROIPixelNum];
	__shared__ float D0[ThreadsPerBlock][FitParaNum*FitParaNum];	// inv of matrix hessian 

																	// avoid bank conflict
	__shared__ float Ininf[FitParaNum][ThreadsPerBlock]; // cur position
	__shared__ float grad[FitParaNum][ThreadsPerBlock];  // gradient
	__shared__ float d0[FitParaNum][ThreadsPerBlock];	// direction


	int gid = threadIdx.x + blockDim.x*blockIdx.x;
	int gid_0 = blockDim.x*blockIdx.x;

	int tid = threadIdx.x;

	gid = min(gid, MultiFitFluoNum);

	int CurFluoID = d_MultiFitFluoPos[gid];


	int XOffset = d_ImageROI[CurFluoID*ROIWholeSize + ROIPixelNum + 0];
	int YOffset = d_ImageROI[CurFluoID*ROIWholeSize + ROIPixelNum + 1];
	int CurFrame = d_ImageROI[CurFluoID*ROIWholeSize + ROIPixelNum + 2] + 65536 * d_ImageROI[CurFluoID*ROIWholeSize + ROIPixelNum + 3];


	//	if (gid >= FluoNum)return; // conflict with the __syncthreads below
	// it's not necessary since d_LocArry size is larger than total fluo number


	float(*pLocArry)[OutParaNumGS2D]; // for parameter array
	pLocArry = (float(*)[OutParaNumGS2D])d_LocArry;


	float(*pD0)[FitParaNum] = (float(*)[FitParaNum])&D0[tid][0];

	// WLE parameter array
	float(*pWLEPara)[WLE_ParaNumber] = (float(*)[WLE_ParaNumber])d_WLEPara;

	float WLE_Weight[ROIPixelNum];

	const float ROICenter = ROISize / 2.0f;


	// load image region from global memory for multi emitter fitting
	for (int fcnt = 0; fcnt < ThreadsPerBlock; fcnt++)
	{
		int CurdId = gid_0 + fcnt;

		CurdId = min(CurdId, MultiFitFluoNum);

		int CurMapID = d_MultiFitFluoPos[CurdId];
		int AddrOffset = CurMapID*ROIWholeSize;

#pragma unroll
		for (int cnt = 0; cnt < LoadLoopNum; cnt++)
		{
			int CurLoadId = cnt*ThreadsPerBlock + tid;

			if (CurLoadId < ROIPixelNum)
			{
				ImageROI[fcnt][CurLoadId] = fmaxf((d_ImageROI[AddrOffset + CurLoadId] - Offset)*kadc, 1.0f);
			}
		}
	}


	int rcnt, ccnt;

	//initial D0
#pragma unroll
	for (rcnt = 0; rcnt < FitParaNum; rcnt++)
	{
#pragma unroll
		for (ccnt = 0; ccnt < FitParaNum; ccnt++)
		{
			if (rcnt == ccnt)pD0[rcnt][ccnt] = 1.0f;
			else pD0[rcnt][ccnt] = 0.0f;
		}
	}


	// why this function impact the time so much? it's should be placed here
	__syncthreads();


	float Distance[FittedFluoNum_2D_3E];
	int MoleculeSel = 0;

	if (gid < MultiFitFluoNum)
	{
		// fixed weighted MLE PSF width
		float WLE_SigmaX = ROISize / 1.9f / 2.35f;
		float WLE_SigmaY = ROISize / 1.9f / 2.35f;

		float NearNeighborDistance = pWLEPara[CurFluoID][WLE_Para_NearDistance];

		// pre-estimation
		PreEstimation_GS2D_3E<ROISize, ROIPixelNum, FitParaNum>(ImageROI, Ininf, tid);


		WLEWeightsCalc<ROISize>(WLE_Weight, WLE_SigmaX, WLE_SigmaY);


		MLELocalization_GS2D_3E<ROISize, ROIPixelNum, FitParaNum_2D_3E, IterateNum_2D_3E, IterateNum_2D_3E_bs>(ImageROI, Ininf, grad, d0, &D0[tid][0], WLE_Weight, tid);


		// remove scaling factor
		Ininf[Fit2D_3E_Peak0][tid] = Ininf[Fit2D_3E_Peak0][tid] * AScalFactor / QE; // PeakPhoton
		Ininf[Fit2D_3E_Peak1][tid] = Ininf[Fit2D_3E_Peak1][tid] * AScalFactor / QE; // PeakPhoton
		Ininf[Fit2D_3E_Peak2][tid] = Ininf[Fit2D_3E_Peak2][tid] * AScalFactor / QE; // PeakPhoton

		float Background = Ininf[Fit2D_3E_Bakg][tid] * BScalFactor / QE; // background
		
		float SimgaX = sqrtf(0.5f / (Ininf[Fit2D_3E_Sigm][tid] * SScalFactor)); // PSF sigma

		// remove nan
		int IsFittingValid = isnan(Background) ? 0 : 1;


		// molecule selection
		for (int cnt = 0; cnt <FittedFluoNum_2D_3E; cnt++)
		{
			float curx = Ininf[3 * cnt + 1][tid];
			float cury = Ininf[3 * cnt + 2][tid];

			if (abs(curx - ROICenter) >= ROISize_Half)curx = -100;
			if (abs(cury - ROICenter) >= ROISize_Half)cury = -100;


			float curPeak = Ininf[3 * cnt + 0][tid];

			Distance[cnt] = sqrtf((curx - ROICenter)*(curx - ROICenter) + (cury - ROICenter)*(cury - ROICenter));
			Distance[cnt] = curPeak*(ROISize - Distance[cnt]);
		}

		MoleculeSel = 0;

		for (int cnt = 0; cnt < FittedFluoNum_2D_3E; cnt++)
		{
			if (Distance[cnt] > Distance[MoleculeSel])
			{
				MoleculeSel = cnt;
			}
		}


		float XPos = Ininf[3 * MoleculeSel + 1][tid] - ROISize_Half + XOffset;
		float YPos = Ininf[3 * MoleculeSel + 2][tid] - ROISize_Half + YOffset;

		float PeakPhoton = Ininf[3 * MoleculeSel + 0][tid];
		float TotalPhoton = PeakPhoton * 2 * 3.141593f*SimgaX * SimgaX;
		float CurSNR = sqrtf(QE) * PeakPhoton / sqrtf(PeakPhoton + Background);


		// select the minimum distance from center?
		if (IsFittingValid && (SimgaX > 0.6f) && (SimgaX <= Simga_Max_Th))
		{

			pLocArry[CurFluoID][Pos_PPho] = PeakPhoton; // peak photon
			pLocArry[CurFluoID][Pos_XPos] = XPos; // may have 0.5 or 1 pixel offset compared with other software
			pLocArry[CurFluoID][Pos_YPos] = YPos; // may have 0.5 or 1 pixel offset compared with other software
			pLocArry[CurFluoID][Pos_ZPos] = 0.0f; //
			pLocArry[CurFluoID][Pos_SigX] = SimgaX; // sigma x
			pLocArry[CurFluoID][Pos_SigY] = SimgaX; // sigma y
			pLocArry[CurFluoID][Pos_TPho] = TotalPhoton;   // total photon
			pLocArry[CurFluoID][Pos_Bakg] = Background; // background
			pLocArry[CurFluoID][Pos_PSNR] = CurSNR;        // peal snr
			pLocArry[CurFluoID][Pos_Frme] = CurFrame; // frame

			// write the second
			// currently only write the center, write others must carefully move the data to keep the molecules from the same frame be stored together

		}
		else
		{
			// invalid point
#pragma unroll
			for (rcnt = 0; rcnt < OutParaNumGS2D; rcnt++)
			{
				pLocArry[CurFluoID][rcnt] = 0;

			}
		}
	}
}


template <int ROISize, int ROIPixelNum, int FitParaNum, int IterateNum, int IterateNum_bs>
__device__ void MLELocalization_GS2D_3E(float ImageROI[][ROIPixelNum], float Ininf[][ThreadsPerBlock], float grad[][ThreadsPerBlock], float d0[][ThreadsPerBlock], float D0[], float* WLE_Weight, int tid)
{
	// adjust d0
	float td0[FitParaNum];
	float td0_total;

	float sk[FitParaNum]; // bfgs quasi-newton method
	float yk[FitParaNum];
	float tgrad[FitParaNum];


	float xd[FitParaNum * 2];

	float ddat[2];
	float dpos[2];


	// initialize
	poissonfGradient_GS2D_3E<ROISize, ROIPixelNum, FitParaNum>(ImageROI, Ininf, grad, WLE_Weight, tid);

#pragma unroll
	for (int rcnt = 0; rcnt < FitParaNum; rcnt++)
	{
		d0[rcnt][tid] = -grad[rcnt][tid];
	}


	// fitting iteration
	for (int itcnt = 0; itcnt<IterateNum; itcnt++)
	{
		td0_total = 0;

		// adjust d0
#pragma unroll
		for (int rcnt = 0; rcnt < FitParaNum; rcnt++)
		{
			td0[rcnt] = abs(d0[rcnt][tid]);
			td0_total += td0[rcnt];
		}
		// reciprocal of mean
		td0_total = ((float)FitParaNum) / td0_total;


#pragma unroll
		for (int rcnt = 0; rcnt < FitParaNum; rcnt++)
		{
			d0[rcnt][tid] = d0[rcnt][tid] * td0_total;

		}


		dpos[0] = 0.00001f; // scale factor left limit, should not equal to 0 and smaller
		dpos[1] = 1.0f; // scale factor right limit, should not lager than 2

		VectorAddMul1<FitParaNum>(&xd[0], Ininf, d0, 0.0001f, tid);
		VectorAddMul1<FitParaNum>(&xd[FitParaNum], Ininf, d0, 1.0f, tid);

		ddat[0] = MLEFit_TargetF_GS2D_3E<ROISize, ROIPixelNum, FitParaNum>(ImageROI, &xd[0], WLE_Weight, tid);
		ddat[1] = MLEFit_TargetF_GS2D_3E<ROISize, ROIPixelNum, FitParaNum>(ImageROI, &xd[FitParaNum], WLE_Weight, tid);
		
		// bisection method to find optimal walk length
		for (int cnt = 0; cnt < IterateNum_bs; cnt++)
		{
			// which part shrink
			int xdsel = (ddat[0] < ddat[1]);


			dpos[xdsel] = (dpos[0] + dpos[1])*0.5f; //  /2.0f which one shift

			if (cnt < IterateNum_bs - 1)
			{
				VectorAddMul1<FitParaNum>(&xd[xdsel*FitParaNum], Ininf, d0, dpos[xdsel], tid);// xd=ininf+d0*scale
				ddat[xdsel] = MLEFit_TargetF_GS2D_3E<ROISize, ROIPixelNum, FitParaNum>(ImageROI, &xd[xdsel*FitParaNum], WLE_Weight, tid);
			}

		}
		float scale = (dpos[0] + dpos[1])*0.5f; //  /2.0f calculated direction

		// sk 		= d0*base;
		// inInf 	= inInf+d0*base;
#pragma unroll
		for (int rcnt = 0; rcnt < FitParaNum; rcnt++)
		{
			sk[rcnt] = d0[rcnt][tid] * scale;
			Ininf[rcnt][tid] = Ininf[rcnt][tid] + sk[rcnt];

		}

		IninfConstrain<FitParaNum>(Ininf, tid); // 


		if (itcnt < IterateNum - 1)
		{
			// tgrad = grad;
#pragma unroll
			for (int rcnt = 0; rcnt < FitParaNum; rcnt++)
			{
				tgrad[rcnt] = grad[rcnt][tid];
			}

			poissonfGradient_GS2D_3E<ROISize, ROIPixelNum, FitParaNum>(ImageROI, Ininf, grad, WLE_Weight, tid);

#pragma unroll
			for (int rcnt = 0; rcnt < FitParaNum; rcnt++)
			{
				yk[rcnt] = grad[rcnt][tid] - tgrad[rcnt];
			}

			ConstructD0<FitParaNum>(D0, sk, yk, tid);
			MatMulVector<FitParaNum>(D0, grad, d0, tid);
		}
	}
}


template <int ROISize, int ROIPixelNum, int FitParaNum>
__device__ void poissonfGradient_GS2D_3E(float ImageROI[][ROIPixelNum], float Ininf[][ThreadsPerBlock], float grad[][ThreadsPerBlock], float* WLE_Weight, int tid)
{
	float tIninf[FitParaNum];
	float tgradn;
	float tgradp;
	int cnt;

#pragma unroll
	for (cnt = 0; cnt < FitParaNum; cnt++)
	{
		tIninf[cnt] = Ininf[cnt][tid];
	}

	for (cnt = 0; cnt<FitParaNum; cnt++)
	{
		tIninf[cnt] = tIninf[cnt] - 0.002f;
		tgradn = MLEFit_TargetF_GS2D_3E<ROISize, ROIPixelNum, FitParaNum>(ImageROI, tIninf, WLE_Weight, tid);

		tIninf[cnt] = tIninf[cnt] + 0.004f;
		tgradp = MLEFit_TargetF_GS2D_3E<ROISize, ROIPixelNum, FitParaNum>(ImageROI, tIninf, WLE_Weight, tid);

		grad[cnt][tid] = (tgradp - tgradn)*250.0f; // /0.004f;
		tIninf[cnt] = tIninf[cnt] - 0.002f;
	}
}



template <int ROISize, int ROIPixelNum, int FitParaNum>
__device__ float MLEFit_TargetF_GS2D_3E(float ImageROI[][ROIPixelNum], float Ininf[], float* WLE_Weight, int tid)
{
	int row, col;

	float rowpos, colpos;
	float TotalLoss = 0;

	float IVal0;
	float IVal1;
	float IVal2;
	float IVal_all;

	float pixval;
	float Loss_t;

	float(*pSubImg)[ROISize] = (float(*)[ROISize])&ImageROI[tid][0];

	float(*pWLE_Weight)[ROISize] = (float(*)[ROISize])WLE_Weight;


#pragma unroll
	for (row = 0; row < FitParaNum; row++)
	{
		if (Ininf[row]< 0.0f)Ininf[row] = 0.0f;
	}


	// slightly improve performance than explicitly define a variable
#define Peak0_Scaled   Ininf[0] * AScalFactor
#define XPos0_Scaled   Ininf[1]
#define YPos0_Scaled   Ininf[2]

#define Peak1_Scaled   Ininf[3] * AScalFactor
#define XPos1_Scaled   Ininf[4]
#define YPos1_Scaled   Ininf[5]

#define Peak2_Scaled   Ininf[6] * AScalFactor
#define XPos2_Scaled   Ininf[7]
#define YPos2_Scaled   Ininf[8]

#define Sigm_Scaled   Ininf[9] * SScalFactor
#define Bakg_Scaled   Ininf[10] * BScalFactor


	for (row = 0; row<ROISize; row++)
	{
		for (col = 0; col<ROISize; col++)
		{
			// pf goal function
			pixval = pSubImg[row][col]; //ImageROI[tid][row*ROISize + col]; // coloffset+

			rowpos = row + 0.5f; // pixel center position
			colpos = col + 0.5f;

			// get model intensity

			IVal0 = Peak0_Scaled * __expf(-((colpos - XPos0_Scaled)*(colpos - XPos0_Scaled) + (rowpos - YPos0_Scaled)*(rowpos - YPos0_Scaled))*Sigm_Scaled);
			IVal1 = Peak1_Scaled * __expf(-((colpos - XPos1_Scaled)*(colpos - XPos1_Scaled) + (rowpos - YPos1_Scaled)*(rowpos - YPos1_Scaled))*Sigm_Scaled);
			IVal2 = Peak2_Scaled * __expf(-((colpos - XPos2_Scaled)*(colpos - XPos2_Scaled) + (rowpos - YPos2_Scaled)*(rowpos - YPos2_Scaled))*Sigm_Scaled);


			IVal_all = IVal0 + IVal1 + IVal2 + Bakg_Scaled;

			Loss_t = IVal_all - pixval - pixval*(__logf(IVal_all) - __logf(pixval));


#if(WLE_ENABLE_MultiEmitterFit == 1)
			// weighted MLE, improve localization precision
			TotalLoss = TotalLoss + pWLE_Weight[row][col] * Loss_t;
#else
			TotalLoss = TotalLoss + Loss_t;

#endif // WLE_ENABLE_MultiEmitterFit

		}
	}

	return TotalLoss;
}


template <int ROISize, int ROIPixelNum, int FitParaNum>
__device__ void PreEstimation_GS2D_3E(float ImageROI[][ROIPixelNum], float Ininf[][ThreadsPerBlock], int tid)
{
	float(*pSubImg)[ROISize] = (float(*)[ROISize])&ImageROI[tid][0];

	int ROISize_Half = ROISize / 2;
	float ROICenter = ROISize / 2.0f;


	// get bkg
	float Bkg = 0;
	float Mean1 = 0;
	float Mean2 = 0;
	float Mean3 = 0;
	float Mean4 = 0;

#pragma unroll
	for (int cnt = 0; cnt < ROISize; cnt++)
	{
		Mean1 += pSubImg[cnt][0];
		Mean2 += pSubImg[cnt][ROISize - 1];
		Mean3 += pSubImg[0][cnt];
		Mean4 += pSubImg[ROISize - 1][cnt];
	}
	Mean1 = min(Mean1, Mean2);
	Mean3 = min(Mean3, Mean4);

	Bkg = (Mean1 + Mean3) / 2 / ROISize;

	float Amp = pSubImg[ROISize_Half][ROISize_Half] - Bkg;

	float SigmaX = ROISize / 2.2f / 2.35f;

	float SigmaX1 = (1.0f / (2.0f*SigmaX*SigmaX));


	// select one regions in four with max intensity
	float SumData[4];

	for (int cnt = 0; cnt < 4; cnt++)
	{
		float SumDat_t = 0;

		int XBias = (cnt % 2)*ROISize_Half;
		int YBias = (cnt / 2)*ROISize_Half;

		for (int r = 0; r <= ROISize_Half; r++)
		{
			for (int c = 0; c <= ROISize_Half; c++)
			{
				SumDat_t += pSubImg[r + YBias][c + XBias];
			}
		}
		SumData[cnt] = SumDat_t;
	}


	int MaxPos1 = 0;
	int MaxPos2 = 0;

	for (int cnt = 0; cnt < 4; cnt++)
	{
		if (SumData[cnt] > SumData[MaxPos1])
		{
			MaxPos1 = cnt;
		}
	}

	SumData[MaxPos1] = 0;

	for (int cnt = 0; cnt < 4; cnt++)
	{
		if (SumData[cnt] > SumData[MaxPos2])
		{
			MaxPos2 = cnt;
		}
	}


	// position estimate of two molecules, the first is given by center, and the other is given by derection with max intensity

	float PosSel[2];

	PosSel[0] = ROICenter - ROISize / 4.6f;
	PosSel[1] = ROICenter + ROISize / 4.6f;


	float XPos1_Ini = PosSel[MaxPos1 % 2];
	float YPos1_Ini = PosSel[MaxPos1 / 2];

	float XPos2_Ini = PosSel[MaxPos2 % 2];
	float YPos2_Ini = PosSel[MaxPos2 / 2];



	// emitter 1 2 3, share the same sigma and background
	Ininf[Fit2D_3E_Peak0][tid] = 0.7f * Amp * rAScalFactor;
	Ininf[Fit2D_3E_XPos0][tid] = ROICenter;
	Ininf[Fit2D_3E_YPos0][tid] = ROICenter;

	Ininf[Fit2D_3E_Peak1][tid] = 0.7f * Amp * rAScalFactor;
	Ininf[Fit2D_3E_XPos1][tid] = XPos1_Ini;
	Ininf[Fit2D_3E_YPos1][tid] = YPos1_Ini;

	Ininf[Fit2D_3E_Peak2][tid] = 0.3f * Amp * rAScalFactor;
	Ininf[Fit2D_3E_XPos2][tid] = XPos2_Ini;
	Ininf[Fit2D_3E_YPos2][tid] = YPos2_Ini;

	Ininf[Fit2D_3E_Sigm][tid] = SigmaX1 * rSScalFactor;

	Ininf[Fit2D_3E_Bakg][tid] = 0.8f * Bkg * rBScalFactor;

}