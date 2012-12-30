

#ifdef MSVC_DEFINES // defined in MSVC project settings, preprocessor section
#define VERSION "12.1.125"
#define I386 
#define WIN32 
#define SWAP_BITFIELDS 
#define _WIN32_WINNT 0x0501
#define WINVER 0x0501
#define NOMINMAX 
#define SESI_LITTLE_ENDIAN 
#define NEED_SPECIALIZATION_STORAGE 
#define AMD64 
#define SIZEOF_VOID_P 8
#define MAKING_DSO
#endif

#include <UT/UT_DSOVersion.h>
#include <UT/UT_Interrupt.h>
#include <UT/UT_Math.h>
#include <UT/UT_Matrix3.h>
#include <UT/UT_Matrix4.h>
#include <GU/GU_Detail.h>
#include <GU/GU_PrimPoly.h>
#include <PRM/PRM_Include.h>
#include <OP/OP_Operator.h>
#include <OP/OP_OperatorTable.h>
#include <SOP/SOP_Guide.h>
#include "aaOceanSOP.h"


void newSopOperator(OP_OperatorTable *table)
{
	table->addOperator(new OP_Operator("aaOceanSOP",
		"aaOceanSOP",
		aaOceanSOP::myConstructor,
		aaOceanSOP::myTemplateList,
		1,
		1,
		0));
}

static PRM_Name names[] = 
{
	PRM_Name("resolution",		"resolution"),
	PRM_Name("seed",			"seed"),
	PRM_Name("oceanScale",		"oceanScale"),

	PRM_Name("velocity",		"velocity"),
	PRM_Name("cutoff",			"cutoff"),
	PRM_Name("windDir",			"windDir"),
	PRM_Name("windAlign",		"windAlign"),

	PRM_Name("damp",			"damp"),
	PRM_Name("waveSpeed",		"waveSpeed"),
	PRM_Name("waveHeight",		"waveHeight"),
	PRM_Name("chop",			"chop"),
	PRM_Name("enableFoam",		"enableFoam"),
	PRM_Name("timeOffset",		"timeOffset"),

};

PRM_Template aaOceanSOP::myTemplateList[] = 
{
	//PRM_Template(PRM_STRING,  1, &PRMgroupName, 0, &SOP_Node::pointGroupMenu),
	PRM_Template(PRM_INT_E,	1, &names[0], PRMfourDefaults, 0, &PRMfrequencyRange),	// resolution
	PRM_Template(PRM_INT_E,	1, &names[1], PRMoneDefaults,  0, &PRMrolloffRange),	// seed
	PRM_Template(PRM_FLT_J,	1, &names[2], PRM100Defaults,  0, &PRMpositiveRange),	// oceanScale

	PRM_Template(PRM_FLT_J,	1, &names[3], PRMfourDefaults, 0, &PRMdivision0Range),	// velocity
	PRM_Template(PRM_FLT_J,	1, &names[4], PRMzeroDefaults, 0, &PRMdivision0Range),	// cutoff
	PRM_Template(PRM_FLT_J,	1, &names[5], PRMzeroDefaults, 0, &PRMangleRange),		// windDir
	PRM_Template(PRM_INT_E,	1, &names[6], PRMzeroDefaults, 0, &PRMdivision0Range),	// windAlign

	PRM_Template(PRM_FLT_J,	1, &names[7], PRMzeroDefaults, 0, &PRMunitRange),		// damp
	PRM_Template(PRM_FLT_J,	1, &names[8], PRMoneDefaults,  0, &PRMdivision0Range),	// waveSpeed
	PRM_Template(PRM_FLT_J,	1, &names[9], PRMoneDefaults,  0, &PRMdivision0Range),	// waveHeight
	PRM_Template(PRM_FLT_J,	1, &names[10], PRMzeroDefaults, 0, &PRMlodRange),		// chop
	PRM_Template(PRM_TOGGLE,    1, &names[11]),										// enable Foam
	PRM_Template(PRM_FLT_J,	1, &names[12], PRMzeroDefaults, 0, &PRMscaleRange),		// timeOffset

	PRM_Template(),
};


OP_Node *aaOceanSOP::myConstructor(OP_Network *net, const char *name, OP_Operator *op)
{
	return new aaOceanSOP(net, name, op);
}

aaOceanSOP::aaOceanSOP(OP_Network *net, const char *name, OP_Operator *op)
	: SOP_Node(net, name, op), myGroup(0)
{
	// Make sure to flag that we can supply a guide geometry
	pOcean = new aaOcean;
	mySopFlags.setNeedGuide1(0);
}

aaOceanSOP::~aaOceanSOP() 
{
	if(pOcean)
		delete pOcean;
}


OP_ERROR aaOceanSOP::cookMySop(OP_Context &context)
{
	fpreal		now		= context.getTime();
	int			npts	= gdp->points().entries();
	UT_Vector3	UVvalue;
	UT_Vector4	Pvalue;


	// Before we do anything, we must lock our inputs.  Before returning,
	//	we have to make sure that the inputs get unlocked.
	if (lockInputs(context) >= UT_ERROR_ABORT)
		return error();

	duplicateSource(0, context);

	// Flag the SOP as being time dependent (i.e. cook on time changes)
	flags().timeDep = 1;

	pOcean->input(	RESOLUTION(), 
					SEED(),
					OCEANSCALE(now), 
					VELOCITY(now), 
					CUTOFF(now), 
					WINDDIR(now), 
					WINDALIGN(), 
					DAMP(now), 
					WAVESPEED(now), 
					WAVEHEIGHT(now),
					CHOP(now), 
					now,
					(bool)ENABLEFOAM());

	if(pOcean->isValid())
	{
		#pragma omp parallel for private(UVvalue, Pvalue)
		for (int i = 0; i < npts; ++i)
		{
			GEO_AttributeHandle	UVhandle = gdp->getAttribute(GEO_POINT_DICT,"uv");

			if(UVhandle.isAttributeValid())
			{
				GEO_AttributeHandle	Phandle  = gdp->getAttribute(GEO_POINT_DICT, "P");

				UVhandle.setElement(gdp->points()(i));
				UVvalue = UVhandle.getV3();

				Phandle.setElement(gdp->points()(i));
				Pvalue = Phandle.getV3();

				Pvalue.y() += pOcean->getOceanData(UVvalue.x(), UVvalue.y(), aaOcean::eHEIGHTFIELD, FALSE);
				if(pOcean->isChoppy())
				{
					Pvalue.x() += pOcean->getOceanData(UVvalue.x(), UVvalue.y(), aaOcean::eCHOPX, FALSE);
					Pvalue.z() += pOcean->getOceanData(UVvalue.x(), UVvalue.y(), aaOcean::eCHOPZ, FALSE);
				}
				Phandle.setV3(Pvalue);
			}
		}
	}
	unlockInputs();
	return error();
}


const char *aaOceanSOP::inputLabel(unsigned) const
{
	return "UV'ed Geometry to simulate ocean on";
}