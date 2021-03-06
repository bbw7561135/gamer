#include "GAMER.h"

#ifdef STAR_FORMATION


struct drand48_data *drand_buf = NULL;

//###: HARD-CODED FIELDS
extern bool AGORA_UseMetal;




//-------------------------------------------------------------------------------------------------------
// Function    :  SF_CreateStar
// Description :  Interface for invoking various star-formation routines
//
// Note        :  1. Invoked by EvolveLevel()
//                2. Allocate NT thread-safe random number generators the first time this function is called,
//                   where NT is the number of OpenMP threads
//                   --> Must be freed by calling SF_FreeRNG() manually
//
// Parameter   :  lv      : Target refinement level
//                TimeNew : Current physical time (after advancing solution by dt)
//                dt      : Time interval to advance solution
//                          --> Currently this function does not distinguish dt and the physical time interval (dTime)
//-------------------------------------------------------------------------------------------------------
void SF_CreateStar( const int lv, const real TimeNew, const real dt )
{

// only form stars on levels above the given minimum level
   if ( lv < SF_CREATE_STAR_MIN_LEVEL )   return;


// initialiez the random number generators the first time this function is called
   static bool FirstTime = true;

   if ( FirstTime )
   {
      int NT;
#     ifdef OPENMP
#     pragma omp parallel
#     pragma omp master
      {  NT = omp_get_num_threads();  }
#     else
      {  NT = 1;                      }
#     endif

      drand_buf = new drand48_data [NT];

//    make sure that different threads in different MPI ranks have different random seeds
//    --> even when SF_CREATE_STAR_DET_RANDOM is enabled, we still set random seeds here just in case that not all
//        star formation methods support SF_CREATE_STAR_DET_RANDOM
      for (int t=0; t<NT; t++)   srand48_r(  SF_CREATE_STAR_RSEED + MPI_Rank*1000 + t, drand_buf + t  );

      FirstTime = false;
   }


//###: HARD-CODED FIELDS
// determine if metallicity is included
// --> currently it's used by the AGORA_IsolatedGalaxy test only (but will be made more general-purpose soon)
   const bool UseMetal = AGORA_UseMetal;


// invoke the target star-formation method
   switch ( SF_CREATE_STAR_SCHEME )
   {
#     if ( MODEL==HYDRO  ||  MODEL==MHD )
      case SF_CREATE_STAR_SCHEME_AGORA:   SF_CreateStar_AGORA( lv, TimeNew, dt, drand_buf, SF_CREATE_STAR_MIN_GAS_DENS,
                                                               SF_CREATE_STAR_MASS_EFF, SF_CREATE_STAR_MIN_STAR_MASS,
                                                               SF_CREATE_STAR_MAX_STAR_MFRAC, SF_CREATE_STAR_DET_RANDOM,
                                                               UseMetal );
      break;
#     endif

      case SF_CREATE_STAR_SCHEME_NONE:    return;
      break;

      default :
         Aux_Error( ERROR_INFO, "incorrect parameter %s = %d !!\n", "SF_CREATE_STAR_SCHEME", SF_CREATE_STAR_SCHEME );
   } // switch ( SF_CREATE_STAR_SCHEME )

} // FUNCTION : SF_CreateStar



//-------------------------------------------------------------------------------------------------------
// Function    :  SF_FreeRNG
// Description :  Free the random number generator allocated by SF_CreateStar()
//
// Note        :  1. Invoked by End_MemFree()
//
// Parameter   :  None
//
// Return      :  None
//-------------------------------------------------------------------------------------------------------
void SF_FreeRNG()
{

   delete [] drand_buf;
   drand_buf = NULL;

} // FUNCTION : SF_FreeRNG



#endif // #ifdef STAR_FORMATION
