#include "GAMER.h"

#if (  defined PARTICLE  &&  defined STAR_FORMATION  &&  ( MODEL==HYDRO || MODEL==MHD )  )


#ifdef GRAVITY
#include "CUPOT.h"
extern double ExtPot_AuxArray[EXT_POT_NAUX_MAX];
extern double ExtAcc_AuxArray[EXT_ACC_NAUX_MAX];
#endif




//-------------------------------------------------------------------------------------------------------
// Function    :  SF_CreateStar_AGORA
// Description :  Create new star particles stochastically using the presription suggested by the AGORA project
//
// Note        :  1. Ref: (1) Nathan Goldbaum, et al., 2015, ApJ, 814, 131 (arXiv: 1510.08458), sec. 2.4
//                        (2) Ji-hoon Kim, et al., 2016, ApJ, 833, 202 (arXiv: 1610.03066), sec. 3.2
//                2. One must turn on STORE_POT_GHOST When adopting STORE_PAR_ACC
//                   --> It is because, currently, this function always uses the pot_ext[] array of each patch
//                       to calculate the gravitationally acceleration of the new star particles
//                3. One must invoke Buf_GetBufferData( ..., _TOTAL, ... ) after calling this function
//                4. Currently this function does not check whether the cell mass exceeds the Jeans mass
//                   --> Ref: "jeanmass" in star_maker_ssn.F of Enzo
//
// Parameter   :  lv           : Target refinement level
//                TimeNew      : Current physical time (after advancing solution by dt)
//                dt           : Time interval to advance solution
//                               --> Currently this function does not distinguish dt and the physical time interval (dTime)
//                               --> Does NOT support COMOVING yet
//                drand_buf    : Buffer for the reentrant and thread-safe random number generator drand48_r()
//                GasDensThres : Minimum gas density for creating star particles                (--> "SF_CREATE_STAR_MIN_GAS_DENS"  )
//                Efficiency   : Gas-to-star mass efficiency                                    (--> "SF_CREATE_STAR_MASS_EFF"      )
//                MinStarMass  : Minimum star particle mass for the stochastical star formation (--> "SF_CREATE_STAR_MIN_STAR_MASS" )
//                MaxStarMFrac : Maximum gas mass fraction allowed to convert to stars          (--> "SF_CREATE_STAR_MAX_STAR_MFRAC")
//                DetRandom    : Make random numbers determinisitic                             (--> "SF_CREATE_STAR_DET_RANDOM"    )
//                UseMetal     : Store the metal mass fraction in star particles
//
// Return      :  1. Particle repository will be updated
//                2. fluid[] array of gas will be updated
//-------------------------------------------------------------------------------------------------------
void SF_CreateStar_AGORA( const int lv, const real TimeNew, const real dt, struct drand48_data *drand_buf,
                          const real GasDensThres, const real Efficiency, const real MinStarMass, const real MaxStarMFrac,
                          const bool DetRandom, const bool UseMetal )
{

// check
#  if ( defined STORE_PAR_ACC  &&  !defined STORE_POT_GHOST )
#     error : STAR_FORMATION + STORE_PAR_ACC must work with STORE_POT_GHOST !!
#  endif

#  ifndef GRAVITY
#     error : must turn on GRAVITY for SF_CreateStar_AGORA() !!
#  endif

#  ifdef COMOVING
#     error : SF_CreateStar_AGORA() does not support COMOVING yet !!
#  endif

   if ( UseMetal )
   {
      if ( PAR_NPASSIVE != 2 )
         Aux_Error( ERROR_INFO, "currently the metal field is hard coded for UseMetal and thus PAR_NPASSIVE (%d) must be 2 !!\n",
                    PAR_NPASSIVE );
   }

   else
   {
      if ( PAR_NPASSIVE != 1 )
         Aux_Error( ERROR_INFO, "currently the passive particle attributes must be hard coded when adding the new particles !!\n" );
   }


// constant parameters
   const double dh             = amr->dh[lv];
   const real   dv             = CUBE( dh );
   const int    FluSg          = amr->FluSg[lv];
   const int    PotSg          = amr->PotSg[lv];
   const real   Coeff_FreeFall = SQRT( (32.0*NEWTON_G)/(3.0*M_PI) );
   const real  _MinStarMass    = (real)1.0 / MinStarMass;
   const real   Eff_times_dt   = Efficiency*dt;
// const real   GraConst       = ( OPT__GRA_P5_GRADIENT ) ? -1.0/(12.0*dh) : -1.0/(2.0*dh);
   const real   GraConst       = ( false                ) ? -1.0/(12.0*dh) : -1.0/(2.0*dh); // P5 is NOT supported yet


// start of OpenMP parallel region
#  pragma omp parallel
   {

// thread-private variables
#  ifdef OPENMP
   const int TID = omp_get_thread_num();
#  else
   const int TID = 0;
#  endif

   double x0, y0, z0, x, y, z;
   real   GasDens, _GasDens, GasMass, _Time_FreeFall, StarMFrac, StarMass, GasMFracLeft;
   real   (*fluid)[PS1][PS1][PS1]      = NULL;
   real   (*pot_ext)[GRA_NXT][GRA_NXT] = NULL;

   const int MaxNewParPerPatch = CUBE(PS1);
   real   (*NewParVar    )[PAR_NVAR    ] = new real [MaxNewParPerPatch][PAR_NVAR    ];
   real   (*NewParPassive)[PAR_NPASSIVE] = new real [MaxNewParPerPatch][PAR_NPASSIVE];
   long    *NewParID                     = new long [MaxNewParPerPatch];

   int NNewPar;


// loop over all real patches
// use static schedule to ensure bitwise reproducibility when running with the same numbers of OpenMP threads and MPI ranks
// --> bitwise reproducibility will still break when running with different numbers of OpenMP threads and/or MPI ranks
//     unless BITWISE_REPRODUCIBILITY is enabled
#  pragma omp for schedule( static )
   for (int PID=0; PID<amr->NPatchComma[lv][1]; PID++)
   {
//    skip non-leaf patches
      if ( amr->patch[0][lv][PID]->son != -1 )  continue;


//    to get deterministic and different random numbers for all patches, reset the random seed of each patch according to
//    its location and time
//    --> patches at different time and/or AMR levels may still have the same random seeds...
      if ( DetRandom )
      {
//       the factor "1.0e6" in the end is just to make random seeds at different times more different, especially for
//       extremely small time-step
         const long RSeed = SF_CREATE_STAR_RSEED + amr->patch[0][lv][PID]->LB_Idx + long(TimeNew*UNIT_T/Const_yr*1.0e6);
         srand48_r( RSeed, drand_buf+TID );
      }


      fluid   = amr->patch[FluSg][lv][PID]->fluid;
      pot_ext = amr->patch[PotSg][lv][PID]->pot_ext;
      x0      = amr->patch[0][lv][PID]->EdgeL[0] + 0.5*dh;
      y0      = amr->patch[0][lv][PID]->EdgeL[1] + 0.5*dh;
      z0      = amr->patch[0][lv][PID]->EdgeL[2] + 0.5*dh;
      NNewPar = 0;

      for (int k=0; k<PS1; k++)
      for (int j=0; j<PS1; j++)
      for (int i=0; i<PS1; i++)
      {

//       1. check the star formation criteria
//       ===========================================================================================================
         GasDens = fluid[DENS][k][j][i];
         GasMass = GasDens*dv;

//       1-1. create star particles only if the gas density exceeds the given threshold
         if ( GasDens < GasDensThres )    continue;


//       1-2. estimate the gas free-fall time
//       --> consider only the gas density under the assumption that the dark matter doesn't collapse
         _Time_FreeFall = Coeff_FreeFall * SQRT( GasDens );


//       1-3. estimate the gas mass fraction to convert to stars
         StarMFrac = Eff_times_dt*_Time_FreeFall;
         StarMass  = GasMass*StarMFrac;


//       1-4. stochastic star formation
//       --> if the star particle mass (StarMass) is below the minimum mass (MinStarMass), we create a
//           new star particle with a mass of MinStarMass and a probability of StarMass/MinStarMass
//       --> Eq. [5] in Goldbaum et al. (2015)
         if ( StarMass < MinStarMass )
         {
            double Random;
            drand48_r( drand_buf+TID, &Random );   // ensure different threads use different drand_buf

            if ( (real)Random < StarMass*_MinStarMass )  StarMFrac = MinStarMass / GasMass;
            else                                         continue;
         }


//       1-5. check the maximum gas mass fraction allowed to convert to stars
         StarMFrac = MIN( StarMFrac, MaxStarMFrac );
         StarMass  = GasMass*StarMFrac;



//       2. store the information of new star particles
//       --> we will not create these new particles until looping over all cells in a patch in order to reduce
//           the OpenMP synchronization overhead
//       ===========================================================================================================
//       check
#        ifdef GAMER_DEBUG
         if ( NNewPar >= MaxNewParPerPatch )
            Aux_Error( ERROR_INFO, "NNewPar (%d) >= MaxNewParPerPatch (%d) !!\n", NNewPar, MaxNewParPerPatch );
#        endif

//       2-1. intrinsic attributes
         _GasDens = (real)1.0 / GasDens;
         x        = x0 + i*dh;
         y        = y0 + j*dh;
         z        = z0 + k*dh;

         NewParVar[NNewPar][PAR_MASS] = StarMass;
         NewParVar[NNewPar][PAR_POSX] = x;
         NewParVar[NNewPar][PAR_POSY] = y;
         NewParVar[NNewPar][PAR_POSZ] = z;
         NewParVar[NNewPar][PAR_VELX] = fluid[MOMX][k][j][i]*_GasDens;
         NewParVar[NNewPar][PAR_VELY] = fluid[MOMY][k][j][i]*_GasDens;
         NewParVar[NNewPar][PAR_VELZ] = fluid[MOMZ][k][j][i]*_GasDens;
         NewParVar[NNewPar][PAR_TIME] = TimeNew;

//       particle acceleration
#        ifdef STORE_PAR_ACC
         real pot_xm = (real)0.0;
         real pot_xp = (real)0.0;
         real pot_ym = (real)0.0;
         real pot_yp = (real)0.0;
         real pot_zm = (real)0.0;
         real pot_zp = (real)0.0;

//       self-gravity potential
         if ( OPT__GRAVITY_TYPE == GRAVITY_SELF  ||  OPT__GRAVITY_TYPE == GRAVITY_BOTH )
         {
            const int ii = i + GRA_GHOST_SIZE;
            const int jj = j + GRA_GHOST_SIZE;
            const int kk = k + GRA_GHOST_SIZE;

            pot_xm = pot_ext[kk  ][jj  ][ii-1];
            pot_xp = pot_ext[kk  ][jj  ][ii+1];
            pot_ym = pot_ext[kk  ][jj-1][ii  ];
            pot_yp = pot_ext[kk  ][jj+1][ii  ];
            pot_zm = pot_ext[kk-1][jj  ][ii  ];
            pot_zp = pot_ext[kk+1][jj  ][ii  ];
         }

//       external potential (currently useful only for ELBDM; always work with OPT__GRAVITY_TYPE == GRAVITY_SELF)
         if ( OPT__EXTERNAL_POT )
         {
            pot_xm += CPU_ExternalPot( x-dh, y,    z,    TimeNew, ExtPot_AuxArray );
            pot_xp += CPU_ExternalPot( x+dh, y,    z,    TimeNew, ExtPot_AuxArray );
            pot_ym += CPU_ExternalPot( x,    y-dh, z,    TimeNew, ExtPot_AuxArray );
            pot_yp += CPU_ExternalPot( x,    y+dh, z,    TimeNew, ExtPot_AuxArray );
            pot_zm += CPU_ExternalPot( x,    y,    z-dh, TimeNew, ExtPot_AuxArray );
            pot_zp += CPU_ExternalPot( x,    y,    z+dh, TimeNew, ExtPot_AuxArray );
         }

//       external acceleration (currently useful only for HYDRO)
         real GasAcc[3] = { (real)0.0, (real)0.0, (real)0.0 };

         if ( OPT__GRAVITY_TYPE == GRAVITY_EXTERNAL  ||  OPT__GRAVITY_TYPE == GRAVITY_BOTH )
            CPU_ExternalAcc( GasAcc, x, y, z, TimeNew, ExtAcc_AuxArray );

//       self-gravity
         if ( OPT__GRAVITY_TYPE == GRAVITY_SELF  ||  OPT__GRAVITY_TYPE == GRAVITY_BOTH )
         {
            GasAcc[0] += GraConst*( pot_xp - pot_xm );
            GasAcc[1] += GraConst*( pot_yp - pot_ym );
            GasAcc[2] += GraConst*( pot_zp - pot_zm );
         }

         NewParVar[NNewPar][PAR_ACCX] = GasAcc[0];
         NewParVar[NNewPar][PAR_ACCY] = GasAcc[1];
         NewParVar[NNewPar][PAR_ACCZ] = GasAcc[2];
#        endif // ifdef STORE_PAR_ACC


//       2-2. passive attributes
//###: HARD-CODED FIELDS
//       note that we store the metal mass **fraction** instead of density in particles
//       currently the metal field is hard coded ... ugh!
         if ( UseMetal )
         NewParPassive[NNewPar][PAR_METAL_FRAC   ] = fluid[METAL][k][j][i] * _GasDens;

         NewParPassive[NNewPar][PAR_CREATION_TIME] = TimeNew;

         NNewPar ++;



//       3. remove the gas that has been converted to stars
//       ===========================================================================================================
         GasMFracLeft = (real)1.0 - StarMFrac;

         for (int v=0; v<NCOMP_TOTAL; v++)   fluid[v][k][j][i] *= GasMFracLeft;
      } // i,j,k



//    4. create new star particles
//    ===========================================================================================================
//    use OpenMP critical construct since both amr->Par->AddOneParticle() and amr->patch[0][lv][PID]->AddParticle()
//    will modify some global variables
//    --> note that the order of which thread calls amr->Par->AddOneParticle() is nondeterministic and may change from run to run
//        --> order of particles stored in the particle repository (i.e., their particle ID) may change from run to run
//        --> particle text file may change from run to run since it's dumped according to the order of particle ID
//    --> but it's not an issue since the actual data of each particle will not be affected
#     pragma omp critical
      {
//       4-1. add particles to the particle repository
         for (int p=0; p<NNewPar; p++)
            NewParID[p] = amr->Par->AddOneParticle( NewParVar[p], NewParPassive[p] );


//       4-2. add particles to the patch
#        ifdef DEBUG_PARTICLE
//       do not set ParPos too early since pointers to the particle repository (e.g., amr->Par->PosX)
//       may change after calling amr->Par->AddOneParticle()
         const real *ParPos[3] = { amr->Par->PosX, amr->Par->PosY, amr->Par->PosZ };
         char Comment[100];
         sprintf( Comment, "%s", __FUNCTION__ );

         amr->patch[0][lv][PID]->AddParticle( NNewPar, NewParID, &amr->Par->NPar_Lv[lv],
                                              ParPos, amr->Par->NPar_AcPlusInac, Comment );
#        else
         amr->patch[0][lv][PID]->AddParticle( NNewPar, NewParID, &amr->Par->NPar_Lv[lv] );
#        endif
      } // pragma omp critical

   } // for (int PID=0; PID<amr->NPatchComma[lv][1]; PID++)

// free memory
   delete [] NewParVar;
   delete [] NewParPassive;
   delete [] NewParID;

   } // end of OpenMP parallel region


// get the total number of active particles in all MPI ranks
   MPI_Allreduce( &amr->Par->NPar_Active, &amr->Par->NPar_Active_AllRank, 1, MPI_LONG, MPI_SUM, MPI_COMM_WORLD );

} // FUNCTION : SF_CreateStar_AGORA



#endif // #if (  defined PARTICLE  &&  defined STAR_FORMATION  &&  ( MODEL==HYDRO || MODEL==MHD )  )
