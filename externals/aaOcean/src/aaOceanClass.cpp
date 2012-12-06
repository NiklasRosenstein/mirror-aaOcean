// aaOcean v2.5
// Author: Amaan Akram 
// www.amaanakram.com
// aaOcean is free software and can be redistributed and modified under the terms of the 
// GNU General Public License (Version 3) as provided by the Free Software Foundation.
// GNU General Public License http://www.gnu.org/licenses/gpl.html

#ifndef AAOCEANCLASS_CPP
#define AAOCEANCLASS_CPP

#include <cmath>
#include <omp.h>
#include <climits>
#include <float.h>
#include <string.h>
#include "constants.h"
#include "functionLib.h"
#include "agnerFog/sfmt.cpp" 
#include "agnerFog/stocc.h"
#include "agnerFog/stoc1.cpp"
#include "agnerFog/userintf.cpp"
#include "aaOceanClass.h"

#define HEIGHTFIELD 0
#define CHOPX 1
#define CHOPZ 2
#define FOAM  3

aaOcean::aaOcean() :
	// input variables
    m_resolution(0),
	m_seed(0),
	m_windAlign(0),			
	m_velocity(-1.0f),
	m_windDir(-1.0f),
	m_cutoff(-1.0f),
	m_damp(-1.0f),
	m_oceanScale(-1.0f),
	m_chopAmount(-1.0f),
	m_waveHeight(-1.0f),
	m_waveSpeed(-1.0f),
	m_time(-1.0f),
	m_fmin(FLT_MAX),
	m_fmax(-FLT_MAX),

	// working arrays
	m_xCoord(0),
	m_zCoord(0),
	m_hokReal(0),
	m_hokImag(0),
	m_hktReal(0),
	m_hktImag(0),
	m_kX(0),
	m_kZ(0),
	m_omega(0),
	m_rand1(0),
	m_rand2(0),
	m_eigenPlusX(0),
	m_eigenPlusZ(0),
	m_eigenMinusX(0),
	m_eigenMinusZ(0),

	// bools to check ocean state
	m_isAllocated(0),
	m_isValid(0),
	m_isFoamAllocated(0),
	m_isSplashAllocated(0),
	m_doHoK(0),
	m_doSetup(0),
	m_doChop(0),
	m_doFoam(0),
	
	// fftw arrays
	m_fft_htField(0),
	m_fft_chopX(0),
	m_fft_chopZ(0),
	m_fft_jxx(0),
	m_fft_jzz(0),
	m_fft_jxz(0)
{
	strcpy (m_state, "[aaOcean Core] Default initialized value");
}

aaOcean::aaOcean(const aaOcean &cpy)
{

}

aaOcean::~aaOcean()
{
	clearArrays();
	
}

void aaOcean::input(int resolution, ULONG seed, float oceanScale, float velocity, 
					float cutoff, float windDir, int windAlign, float damp, float waveSpeed, 
					float waveHeight, float chopAmount, float time, bool doFoam, bool powTwoConversion = 1)
{
	m_isValid = FALSE;

	if(powTwoConversion)
		resolution	= (int)pow(2.0f, (4 + resolution)); // forcing to be power of two
	oceanScale	= maximum<float>(oceanScale, 0.00001f);	// clamping to minimum value
	velocity	= maximum<float>(((velocity  * velocity) / aa_GRAVITY), 0.00001f); // clamping to minimum value
	cutoff		= fabs(cutoff * 0.01f);
	windDir		= windDir * aa_PIBY180; // to radians
	windAlign	= maximum<int>(((windAlign + 1) * 2), 2); // forcing to even numbers
	damp		= minimum<float>(damp, 1.0f); // clamping to a maximum value of 1

	waveHeight *= 0.01f; // scaled for better UI control
	chopAmount *= 0.01f; // scaled for better UI control

	m_time			= time;
	m_waveSpeed		= waveSpeed;
	m_waveHeight	= waveHeight;
	m_doFoam		= doFoam;

	if(chopAmount > 0.000001f)
	{
		m_chopAmount = chopAmount;
		m_doChop = TRUE;
	}
	else
	{
		m_doChop = FALSE;
		m_chopAmount = 0.0f;
	}

	if( m_oceanScale	!= oceanScale	||
		m_windDir		!= windDir		||
		m_cutoff		!= cutoff		||
		m_velocity		!= velocity		||
		m_windAlign		!= windAlign	||
		m_damp			!= damp		)
	{
		m_oceanScale	= oceanScale;
		m_windDir		= windDir;
		m_cutoff		= cutoff;
		m_velocity		= velocity;
		m_windAlign		= windAlign;
		m_damp			= damp;
		m_doHoK		    = TRUE;
	}

	if(m_seed != seed)
	{
		m_seed	= seed;
		m_doHoK	= TRUE;
		if(m_resolution == resolution)
			m_doSetup = TRUE; 
	}

	if(reInit(resolution))
		prepareOcean();
}

bool aaOcean::isValid()
{
	return m_isValid;
}

bool aaOcean::isChoppy()
{
	return m_doChop;
}

int aaOcean::getResolution()
{
	return m_resolution;
}

bool aaOcean::reInit(int resolution)
{
	if(((resolution & (resolution - 1)) != 0) || resolution <= 0) // bad input -- not power of 2
	{	
		sprintf(m_state,"[aaOcean Core] Invalid point resolution of %d. Please select a power-of-2 subdivision value", resolution);
		m_isValid = FALSE;
	}
	else
	{
		// If ocean resolution has changed, or if we are creating an ocean from scratch
		// Flush any existing arrays and allocate them with the new array size (resolution)
		if(m_resolution != resolution || !m_isAllocated )
		{
			m_resolution = resolution;
			allocateBaseArrays();				
			m_doHoK  = TRUE;
			setupGrid();
			sprintf(m_state,"[aaOcean Core] Building ocean shader with resolution %dx%d", resolution, resolution);
		}
		// if doSetup has been set to True because of a change in Seed
		if(m_doSetup)
			setupGrid();

		m_isValid = TRUE;
	}
	return m_isValid;
}

void aaOcean::prepareOcean()
{
	if( m_doHoK )
		evaluateHokData();

	evaluateHieghtField();
	makeTileable(m_fft_htField);

	if(m_doChop)
	{
		evaluateChopField();
		makeTileable(m_fft_chopX);
		makeTileable(m_fft_chopZ);
	}

	if(m_doFoam)
	{
		if(!m_isFoamAllocated)
			allocateFoamArrays();
		if(!m_isSplashAllocated)
			allocateSplashArrays();

		evaluateJacobians();
	}
}

void aaOcean::allocateBaseArrays()
{
	if(m_isAllocated) 
		clearArrays();
	m_xCoord	= (int*)   malloc(m_resolution * m_resolution * sizeof(int)); 
	m_zCoord	= (int*)   malloc(m_resolution * m_resolution * sizeof(int)); 
	
	m_hokReal	= (float*) malloc(m_resolution * m_resolution * sizeof(float)); 
	m_hokImag	= (float*) malloc(m_resolution * m_resolution * sizeof(float)); 
	m_hktReal	= (float*) malloc(m_resolution * m_resolution * sizeof(float)); 
	m_hktImag	= (float*) malloc(m_resolution * m_resolution * sizeof(float)); 
	m_kX		= (float*) malloc(m_resolution * m_resolution * sizeof(float)); 
	m_kZ		= (float*) malloc(m_resolution * m_resolution * sizeof(float)); 
	m_omega		= (float*) malloc(m_resolution * m_resolution * sizeof(float)); 
	m_rand1		= (float*) malloc(m_resolution * m_resolution * sizeof(float)); 
	m_rand2		= (float*) malloc(m_resolution * m_resolution * sizeof(float)); 
	
	if(m_resolution > 254)
	{
		int threads = omp_get_num_procs();
		fftwf_plan_with_nthreads(threads);
	}
	else
		fftwf_plan_with_nthreads(1);

	m_fft_htField	= (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * ((m_resolution+1) * (m_resolution+1)));  
	m_fft_chopX		= (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * ((m_resolution+1) * (m_resolution+1)));  
	m_fft_chopZ		= (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * ((m_resolution+1) * (m_resolution+1))); 

	m_planHeightField	= fftwf_plan_dft_2d(m_resolution,m_resolution,m_fft_htField,m_fft_htField,1, FFTW_ESTIMATE);
	m_planChopX			= fftwf_plan_dft_2d(m_resolution,m_resolution,m_fft_chopX ,m_fft_chopX	 ,1, FFTW_ESTIMATE);
	m_planChopZ			= fftwf_plan_dft_2d(m_resolution,m_resolution,m_fft_chopZ ,m_fft_chopZ	 ,1, FFTW_ESTIMATE);
	m_isAllocated		= TRUE;
}

 void aaOcean::allocateFoamArrays()
{
	m_fft_jxx = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * ((m_resolution+1) * (m_resolution+1))); 
	m_fft_jxz = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * ((m_resolution+1) * (m_resolution+1))); 
	m_fft_jzz = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * ((m_resolution+1) * (m_resolution+1))); 

	m_planJxx = fftwf_plan_dft_2d(m_resolution, m_resolution, m_fft_jxx, m_fft_jxx, 1, FFTW_ESTIMATE);
	m_planJxz = fftwf_plan_dft_2d(m_resolution, m_resolution, m_fft_jxz, m_fft_jxz, 1, FFTW_ESTIMATE);
	m_planJzz = fftwf_plan_dft_2d(m_resolution, m_resolution, m_fft_jzz, m_fft_jzz, 1, FFTW_ESTIMATE);
	m_isFoamAllocated = TRUE;
}

void aaOcean::allocateSplashArrays()
{
	m_eigenPlusX	= (float*) malloc((m_resolution+1) * (m_resolution+1) * sizeof(float)); 
	m_eigenPlusZ	= (float*) malloc((m_resolution+1) * (m_resolution+1) * sizeof(float)); 
	m_eigenMinusX	= (float*) malloc((m_resolution+1) * (m_resolution+1) * sizeof(float)); 
	m_eigenMinusZ	= (float*) malloc((m_resolution+1) * (m_resolution+1) * sizeof(float)); 
	m_isSplashAllocated = TRUE;
}

void aaOcean::clearResidualArrays()
{
	if(m_rand2)
	{
		free(m_rand2); 
		m_rand2 = FALSE;
	}
	if(m_rand1)
	{
		free(m_rand1); 
		m_rand1 = FALSE;
	}
	if(m_omega)
	{
		free(m_omega); 
		m_omega = FALSE;
	}
	if(m_kZ)
	{
		free(m_kZ); 
		m_kZ = FALSE;
	}
	if(m_kX)
	{
		free(m_kX); 
		m_kX = FALSE;
	}
	if(m_hktImag)
	{
		free(m_hktImag); 
		m_hktImag = FALSE;
	}
	if(m_hktReal)
	{
		free(m_hktReal); 
		m_hktReal = FALSE;
	}
	if(m_hokImag)
	{
		free(m_hokImag); 
		m_hokImag = FALSE;
	}
	if(m_hokReal)
	{
		free(m_hokReal); 
		m_hokReal = FALSE;
	}
	if(m_zCoord)
	{
		free(m_zCoord); 
		m_zCoord = FALSE;
	}
	if(m_xCoord)
	{
		free(m_xCoord); 
		m_xCoord = FALSE;
	}
}

void aaOcean::clearArrays()
{
	if(m_isAllocated)
	{
		if(m_fft_jzz)
		{
			fftwf_destroy_plan(m_planJzz);
			fftwf_free(m_fft_jzz);  
			m_fft_jzz = FALSE;
		}
		if(m_isFoamAllocated)
		{
			if(m_fft_jxz)
			{
				fftwf_destroy_plan(m_planJxz);
				fftwf_free(m_fft_jxz); 
				m_fft_jxz = FALSE;
			}
			m_isFoamAllocated = FALSE;
		}
		if(m_fft_jxx)
		{
			fftwf_destroy_plan(m_planJxx);
			fftwf_free(m_fft_jxx); 
			m_fft_jxx = FALSE;
		}
		if(m_fft_chopZ)
		{
			fftwf_destroy_plan(m_planChopZ);
			fftwf_free(m_fft_chopZ); 
			m_fft_chopZ = FALSE;
		}
		if(m_fft_chopX)
		{
			fftwf_destroy_plan(m_planChopX);
			fftwf_free(m_fft_chopX); 
			m_fft_chopX = FALSE;
		}
		if(m_fft_htField)
		{
			fftwf_destroy_plan(m_planHeightField);
			fftwf_free(m_fft_htField); 
			m_fft_htField = FALSE;
		}
		m_isAllocated = FALSE;
	}
	
	if(m_isSplashAllocated)
	{
		if(m_eigenMinusZ)
		{
			free(m_eigenMinusZ); 
			m_eigenMinusZ = FALSE;
		}
		if(m_eigenMinusX)
		{
			free(m_eigenMinusX); 
			m_eigenMinusX = FALSE;
		}
		if(m_eigenPlusZ)
		{
			free(m_eigenPlusZ); 
			m_eigenPlusZ = FALSE;
		}
		if(m_eigenPlusX)
		{
			free(m_eigenPlusX); 
			m_eigenPlusX = FALSE;
		}
		m_isSplashAllocated = FALSE;
	}
	clearResidualArrays();
	
}

ULONG aaOcean::generateUID(float xCoord, float zCoord)
{
	// a very simple hash function. should probably do a better one at some point
	register float angle  = 0.0f;
	register float length = 0.0f;
	register float id_out;
	ULONG returnVal = 1;

	if (zCoord != 0.0f && xCoord != 0.0f)
	{
		angle = xCoord / sqrt(zCoord * zCoord + xCoord * xCoord); 
		angle = acos(angle);
		angle = RadsToDegs(angle);
		
		if (angle > 180.0f)
			angle = 360.0f - angle;
		
		length = sqrt(zCoord * zCoord + xCoord * xCoord); 
		id_out = (length * length)  +  (length * angle);
		
		if( angle == 0.0f)
			returnVal = (ULONG)(length * length);
		else if (zCoord <= 0.0f)
			returnVal = (ULONG)floor(id_out + 0.5f);
		else
			returnVal = INT_MAX - (ULONG)floor(id_out + 0.5f) ;
	}
	return returnVal;
}

void aaOcean::setupGrid()
{
	if(!m_isAllocated)
		return;
	register const int n = m_resolution;
	register const int half_n = (-n / 2) - ((n-1) / 2);
	register ULONG index, uID; 

	#pragma omp parallel for private(index, uID)
	for(int i = 0; i < n; ++i)
	{
		for(int j = 0; j < n; ++j)
		{
			index = (i * n) + j;

			m_xCoord[index] = half_n + i * 2 ;
			m_zCoord[index] = half_n + j * 2 ;

			uID = (ULONG)generateUID((float)m_xCoord[index], (float)m_zCoord[index]);

			StochasticLib1 sto(uID + (unsigned int)m_seed);
			m_rand1[index] = (float)sto.Normal(0.0f, 1.0f);
			m_rand2[index] = (float)sto.Normal(0.0f, 1.0f);
		}
	}
	m_doSetup = FALSE;
}

 void aaOcean::evaluateHokData()
{
	register float k_sq, k_mag,  k_dot_w, philips, x, z;

	register const int		n		 = m_resolution * m_resolution;
	register const float	k_mult	 = aa_TWOPI / m_oceanScale;
	register const float	L		 = m_velocity;
	register const float	L_sq	 = L * L;
	register const float	windx	 = cos(m_windDir);
	register const float	windz	 = sin(m_windDir);
	
	bool bDamp	= FALSE;
	if (m_damp > 0.0f)
		bDamp = true;

	#pragma omp parallel for private( k_sq, k_mag, k_dot_w, philips, x, z)  
	for(int index = 0; index < n; ++index)
	{
		x = m_kX[index] =  m_xCoord[index] * k_mult; 
		z = m_kZ[index] =  m_zCoord[index] * k_mult;

		//philips spectrum vars
		k_sq		= (x * x) + (z * z);
		k_mag		= 1.0f / sqrt( k_sq );
		k_dot_w		= (x * k_mag) * windx + (z * k_mag) * windz;
		philips		= sqrt((( exp(-1.0f / ( L_sq * k_sq)) * pow(k_dot_w, m_windAlign)) / 
					  (k_sq * k_sq)) * exp(-k_sq * m_cutoff));

		if(bDamp)
		{
			if(k_dot_w < 0.0f)
				philips *= (1.0f - m_damp);
		}		

		m_omega[index]   = sqrt(aa_GRAVITY / k_mag );
		m_hokReal[index] = (aa_INV_SQRTTWO) * (m_rand1[index]) * philips;
		m_hokImag[index] = (aa_INV_SQRTTWO) * (m_rand2[index]) * philips;
	}
	m_doHoK = FALSE;
}

 void aaOcean::evaluateHieghtField()
{
	int  i,j,index, index_rev;
	register float  hokReal, hokImag, hokRealOpp, hokImagOpp, sinwt, coswt;

	float	wt  = m_waveSpeed * m_time;
	const int n = m_resolution;
	const int nn = n * n;
	register const int n_sq = n * n - 1;

	#pragma omp parallel for private(index, index_rev, hokReal, hokImag, hokRealOpp, hokImagOpp, sinwt, coswt)  
	for(index = 0; index < nn; ++index)
	{
		index_rev   = n_sq - index; //tail end 
		hokReal		= m_hokReal[index];
		hokImag		= m_hokImag[index];
		hokRealOpp	= m_hokReal[index_rev];
		hokImagOpp	= m_hokImag[index_rev];

		coswt = cos( m_omega[index] * wt);
		sinwt = sin( m_omega[index] * wt);

		m_hktReal[index]  =	( hokReal    *	coswt )  + 	( hokImag	 *	sinwt )  + 
							( hokRealOpp *	coswt )  -  ( hokImagOpp *	sinwt )  ;  //complex conjugage
				
		m_hktImag[index]  =	(-hokReal	 *	sinwt )  + 	( hokImag	 *	coswt )  +
							( hokRealOpp *	sinwt )  +  ( hokImagOpp *	coswt )  ;  //complex conjugage
		
		m_fft_htField[index][0] = m_hktReal[index];
		m_fft_htField[index][1] = m_hktImag[index];
	}

	fftwf_execute(m_planHeightField);

	#pragma omp parallel for private(i,j)
	for(i = 0; i < n; ++i)
	{
		for(j = 0; j < n; ++j)
			m_fft_htField[(i*n) + j][0] *= isEven(i+j)  * m_waveHeight;
	}
}

 void aaOcean::evaluateChopField()
{
	int  i, j, index;
	register float _kX,_kZ, kMag;
	int n = m_resolution * m_resolution;

	#pragma omp parallel for private( index, _kX, _kZ, kMag)  
	for(index = 0; index < n; ++index)
	{			
		kMag = sqrt(m_kX[index] * m_kX[index] + m_kZ[index] * m_kZ[index]);
		_kX = m_kX[index] / kMag;
		_kZ = m_kZ[index] / kMag;
		
		m_fft_chopX[index][0] =  m_hktImag[index] * _kX;
		m_fft_chopX[index][1] = -m_hktReal[index] * _kX;

		m_fft_chopZ[index][0] =  m_hktImag[index] * _kZ;
		m_fft_chopZ[index][1] = -m_hktReal[index] * _kZ;
	}

	fftwf_execute(m_planChopX);
	fftwf_execute(m_planChopZ);

	n = m_resolution;
	#pragma omp parallel for private(i, j, index)  
	for(i = 0; i < n; ++i)
	{
		for(j = 0; j < n; ++j)
		{
			index = (i*n) + j;
			m_fft_chopX[index][0] *= m_chopAmount * isEven(i+j);
			m_fft_chopZ[index][0] *= m_chopAmount * isEven(i+j);
		}
	}
}

void aaOcean::evaluateJacobians()
{
	int  i, j, index;
	register float _kX, _kZ, kMag, kXZ;
	int n = m_resolution * m_resolution;

	#pragma omp parallel for private( index, _kX, _kZ, kXZ, kMag) 
	for(index = 0; index < n; ++index)
	{			
		kMag = 1.0f / sqrt(m_kX[index] * m_kX[index] + m_kZ[index] * m_kZ[index]);
		_kX  = (m_kX[index] * m_kX[index]) * kMag;
		_kZ  = (m_kZ[index] * m_kZ[index]) * kMag;
		kXZ  = (m_kX[index] * m_kZ[index]) * kMag;

		m_fft_jxx[index][0] =  m_hktReal[index] * _kX;
		m_fft_jxx[index][1] =  m_hktImag[index] * _kX;

		m_fft_jzz[index][0] =  m_hktReal[index] * _kZ;
		m_fft_jzz[index][1] =  m_hktImag[index] * _kZ;

		m_fft_jxz[index][0] =  m_hktReal[index] * kXZ;
		m_fft_jxz[index][1] =  m_hktImag[index] * kXZ;
	}

	fftwf_execute(m_planJxx);
	fftwf_execute(m_planJzz);
	fftwf_execute(m_planJxz);

	n = m_resolution;
	#pragma omp parallel for private(i, j, index)  
	for(i = 0; i < n; ++i)
	{
		for(j = 0; j < n; ++j)
		{
			index = (i*n) + j;
			m_fft_jxx[index][0] = (m_fft_jxx[index][0] * -m_chopAmount * isEven(i+j)) + 1.0f;
			m_fft_jzz[index][0] = (m_fft_jzz[index][0] * -m_chopAmount * isEven(i+j)) + 1.0f;
			m_fft_jxz[index][0] =  m_fft_jxz[index][0] * -m_chopAmount * isEven(i+j);
		}
	}

	register float jPlus, jMinus, qPlus, qMinus, Jxx, Jzz, Jxz;
	#pragma omp parallel for private(index, jPlus, jMinus, qPlus, qMinus, Jxx, Jzz, Jxz)  
	for(index = 0; index < n*n; ++index)
	{
		Jxx = m_fft_jxx[index][0];
		Jzz = m_fft_jzz[index][0];
		Jxz = m_fft_jxz[index][0];

		jPlus	= (0.5f * (Jxx + Jzz))  +  (0.5f * sqrt( ((Jxx - Jzz)*(Jxx - Jzz)) + 4.0f * (Jxz*Jxz) ));
		jMinus	= (0.5f * (Jxx + Jzz))  -  (0.5f * sqrt( ((Jxx - Jzz)*(Jxx - Jzz)) + 4.0f * (Jxz*Jxz) ));
		qPlus	= (jPlus  - Jxx) / Jxz;
		qMinus	= (jMinus - Jxx) / Jxz;

		m_eigenPlusX[index] = 1.0f	    /  sqrt( 1.0f + qPlus * qPlus);
		m_eigenPlusZ[index] = qPlus		/  sqrt( 1.0f + qPlus * qPlus);

		m_eigenMinusX[index] = 1.0f		/  sqrt( 1.0f + qMinus * qMinus);
		m_eigenMinusZ[index] = qMinus	/  sqrt( 1.0f + qMinus * qMinus);

		//store foam back in this array for convenience
		//fft_jxz[index][0] =   (Jxx * Jzz) - (Jxz * Jxz); //original jacobian.
		m_fft_jxz[index][0] =   (Jxz * Jxz) - (Jxx * Jzz) + 1.0f; // invert hack
	}
}

float aaOcean::getOceanData(float uCoord, float vCoord, int TYPE)
{
	// set pointer to the array that we need to interpolate data from
	fftwf_complex *arrayPointer = 0;
	if(TYPE == HEIGHTFIELD)
		arrayPointer = m_fft_htField;
	else if(TYPE == CHOPX)
		arrayPointer = m_fft_chopX;
	else if(TYPE == CHOPZ)
		arrayPointer = m_fft_chopZ;
	else if(TYPE == FOAM)
		arrayPointer = m_fft_jxz;

	float u, v, du, dv = 0;
	int xMinus1, yMinus1, x, y, xPlus1, yPlus1, xPlus2, yPlus2;
	int n = m_resolution;	

	if(vCoord > 1.0f)
		vCoord = vCoord - floor(vCoord);
	if(uCoord > 1.0f)
		uCoord = uCoord - floor(uCoord);
	if(vCoord < 0.0f)
		vCoord = vCoord - floor(vCoord);
	if(uCoord < 0.0f)
		uCoord = uCoord- floor(uCoord);

	u = vCoord * float(m_resolution);
	v = uCoord * float(m_resolution);
	x = (int)floor(u);
	y = (int)floor(v);

	xMinus1 = wrap((x-1), m_resolution);
	yMinus1 = wrap((y-1), m_resolution);	
	x = wrap(x, m_resolution);
	y = wrap(y, m_resolution);		
	xPlus1 = wrap((x+1), m_resolution);	
	yPlus1 = wrap((y+1), m_resolution);	
	xPlus2 = wrap((x+2), m_resolution);	
	yPlus2 = wrap((y+2), m_resolution);	

	du = u - x; 
	dv = v - y;	

	const int index = 1;

	float a1 = catmullRom(	du, 
							arrayPointer[xMinus1 * (m_resolution+1)  + yMinus1][index],
							arrayPointer[x*(m_resolution+1)			 + yMinus1][index],
							arrayPointer[xPlus1*(m_resolution+1)	 + yMinus1][index],
							arrayPointer[xPlus2*(m_resolution+1)	 + yMinus1][index] 
							);

	float b1 = catmullRom(	du, 
							arrayPointer[xMinus1*(m_resolution+1)    +	y][index],
							arrayPointer[x*(m_resolution+1)			 +	y][index],
							arrayPointer[xPlus1*(m_resolution+1)	 +	y][index],
							arrayPointer[xPlus2*(m_resolution+1)	 +	y][index]
							);

	float c1 = catmullRom(	du, 
							arrayPointer[xMinus1*(m_resolution+1)    + yPlus1][index],
							arrayPointer[x*(m_resolution+1)			 + yPlus1][index],
							arrayPointer[xPlus1*(m_resolution+1)	 + yPlus1][index],
							arrayPointer[xPlus2*(m_resolution+1)	 + yPlus1][index] 
							);

	float d1 = catmullRom(	du, 
							arrayPointer[xMinus1*(m_resolution+1)    + yPlus2][index],
							arrayPointer[x*(m_resolution+1)			 + yPlus2][index],
							arrayPointer[xPlus1*(m_resolution+1)	 + yPlus2][index],
							arrayPointer[xPlus2*(m_resolution+1)	 + yPlus2][index]
							);

	return catmullRom(dv,a1,b1,c1,d1);
}
inline float aaOcean::catmullRom(float t, float a, float b, float c, float d)
{
	return  0.5f * ( ( 2.0f * b ) + ( -a + c ) * t + 
			( 2.0f * a - 5.0f * b + 4 * c - d ) * t*t + 
			( -a + 3.0f * b - 3.0f * c + d )* t*t*t );
}

inline int aaOcean::wrap(int x, int n)
{
	if(x > n)
		x = x % (n+1);
	else if(x < 0)
		x = (n+1) + x % (n+1);
	
	return x;
}

void aaOcean::rotateArray(fftwf_complex *&fft_array)
{
	// this function is used sometimes because in some cases
	// there is a 90 degree rotation difference between deformer and ocean
	// which is often fixable by a simple vec(x,y)->vec(-y,x), but if not
	// then this function is used

	int n = m_resolution + 1;
	float tmp;
	for (int i=0; i<n/2; i++)
	{
        for (int j=i; j<n-i-1; j++)
		{
            tmp								 = fft_array[i*n+j][1];
            fft_array[i*n+j][1]				 = fft_array[j*n +(n-i-1)][1];
            fft_array[j*n +(n-i-1)][1]		 = fft_array[(n-i-1)*n +(n-j-1)][1];
            fft_array[(n-i-1)*n +(n-j-1)][1] = fft_array[(n-j-1)*n + i][1];
            fft_array[(n-j-1)*n + i][1]		 = tmp;
        }
	}
}
void aaOcean::makeTileable(fftwf_complex *&fft_array)
{
	// This function is admittedly crap
	// It makes the ocean grid tileable (when it should already be tileable!!)
	// Needlessly inefficient. I should fix this.
	// Also, optimze this from O(N^2) to O(2N) later

	int n = m_resolution;
	int n1 = n + 1;
	int index, i, j;
	
	#pragma omp parallel for private(index,i,j)
	for(i = 0; i< n1; i++)
	{					
		for(j = 0; j< n1; j++)
		{
			index = i*n1 + j;
			if( i<n && j<n) // regular array copy -- what a waste of resources
			{
				fft_array[index][1] = fft_array[i*n+j][0];
			}
			else
			{
				if(i==n)  //copy left-most col to right-most col
				{
					fft_array[index][1] = fft_array[j][0];
				}
				if(j==n) // copy top row to bottom row
				{
					fft_array[index][1] = fft_array[i*n][0];
				}
				if(i==n && j==n) // copy top-left corner to bottom-left
				{
					fft_array[index][1] = fft_array[0][0];
				}
			}
		}
	}
}
#endif  /* AAOCEANCLASS_CPP */