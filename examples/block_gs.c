/*BHEADER**********************************************************************
 * Copyright (c) 2013, Lawrence Livermore National Security, LLC. 
 * Produced at the Lawrence Livermore National Laboratory. Written by 
 * Jacob Schroder, Rob Falgout, Tzanio Kolev, Ulrike Yang, Veselin 
 * Dobrev, et al. LLNL-CODE-660355. All rights reserved.
 * 
 * This file is part of XBraid. For support, post issues to the XBraid Github page.
 * 
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License (as published by the Free Software
 * Foundation) version 2.1 dated February 1999.
 * 
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. See the terms and conditions of the GNU General Public
 * License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 ***********************************************************************EHEADER*/

 /**
 * Example:       advec-diff-omgrit.c
 *
 * Interface:     C
 * 
 * Requires:      only C-language support     
 *
 * Compile with:  make ex-04-adjoint
 *
 * Description:  Solves a simple optimal control problem in time-parallel:
 * 
 *                 min   0.5\int_0^T \int_0^1 (u(x,t)-u0(x))^2+alpha v(x,t)^2 dxdt
 * 
 *                  s.t.  du/dt + du/dx - nu d^2u/dx^2 = v(x,t)
 *                        u(0,t)=u(1,t)=0
 *                                  u(x,0)=u0(x)
 *
 *               Implements a steepest-descent optimization iteration
 *               using fixed step size for design updates.   
 **/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "braid.h"
#include "braid_test.h"
#define PI 3.14159265
#define g(dt,dx) dt/(2*dx)
#define b(dt,dx,nu) nu*dt/(dx*dx)
/*--------------------------------------------------------------------------
 * My App and Vector structures
 *--------------------------------------------------------------------------*/

typedef struct _braid_App_struct
{
   int      myid;        /* Rank of the processor */
   double   alpha;       /* Relaxation parameter for objective function, v(x,t) */
   double   nu;          /* Diffusion coefficent, which we take to be large */
   int      ntime;       /* Total number of time-steps (starting at time 0) */
   int      mspace;      /* Number of space points included in our state vector */
                         /* So including boundaries we have M+2 space points */

   double **w;           /* Adjoint vectors at each time point on my proc */
   double *U0;
   double *ai;
   double *li;


} my_App;


/* Define the state vector at one time-step */
typedef struct _braid_Vector_struct
{
   double *values;     /* Holds the R^M state vector (u_1, u_2,...,u_M) */

} my_Vector;

/*--------------------------------------------------------------------------
 * Vector utility routines
 *--------------------------------------------------------------------------*/

void
vec_create(int size, double **vec_ptr)
{
   *vec_ptr = (double*) malloc( size*sizeof(double) );
}

void
vec_destroy(double *vec)
{
   free(vec);
}

/*------------------------------------*/

void
vec_copy(int size, double *invec, double *outvec)
{
   int i;
   for (i = 0; i < size; i++)
   {
      outvec[i] = invec[i];
   }
}

/*------------------------------------*/

void
vec_axpy(int size, double alpha, double *x, double *y)
{
   int i;
   for (i = 0; i < size; i++)
   {
      y[i] = y[i] + alpha*x[i];
   }
}

/*------------------------------------*/

void
vec_scale(int size, double alpha, double *x)
{
   int i;
   for (i = 0; i < size; i++)
   {
      x[i] = alpha*x[i];
   }
}

/*--------------------------------------------------------------------------
 * KKT component routines
 *--------------------------------------------------------------------------*/

/* This is the K=[A B C] matrix. It acts on a vector in R^M */
/* This function requies that M>=3, but this can easily be fixed later */
void
apply_Phi(double dt, double dx, double nu, int M, double *u, double *l, double *a)
{   
   /* First solve Lw=u (Lw=f) */
   double *w;
   vec_create(M, &w);
   double *f;
   vec_create(M, &f);
   vec_copy(M, u, f);
   w[0]=f[0];
   for (int i = 1; i < M; i++)
   {
      w[i]=f[i]-l[i-1]*w[i-1];
   }

   /* Now solve Ux=w */ 
   double b = g(dt,dx)-b(dt, dx, nu);
   u[M-1]=w[M-1]/a[M-1];
   for (int i = M-2; i >= 0; i--)
   {
      u[i]=(w[i]-b*u[i+1])/a[i];      
   }
}

/*------------------------------------*/

void
apply_PhiAdjoint(double dt, double dx, double nu, int M, double *u, double *l, double *a)
{
   /* First solve U^Tw=u (U^Tw=f) */

   /* Need to change this w to some other letter becuase w is already passed as a parameter of this function */
   double *w;

   vec_create(M, &w);
   double *f;
   vec_create(M, &f);
   vec_copy(M, u, f);
   double b = g(dt,dx)-b(dt, dx, nu);
   w[0]=f[0]/a[0];
   for (int i = 1; i < M; i++)
   {
      w[i]=(f[i]-w[i-1]*b)/a[i];
   }

   /* Now solve L^Tx=w */ 
   
   u[M-1]=w[M-1];
   for (int i = M-2; i >= 0; i--)
   {
      u[i]=w[i]-l[i]*u[i+1];      
   }
}

/*------------------------------------*/

void
apply_A(double dt, double dx, double nu, int M, double *u)
{
   double A = -g(dt,dx)-b(dt,dx,nu);
   double B = 1+2*b(dt,dx,nu);
   double C = g(dt,dx)-b(dt,dx,nu);
   double *uold;
   vec_create(M, &uold);
   vec_copy(M, u, uold);
   u[0]=B*uold[0]+C*uold[1];
   u[M-1]=A*uold[M-2]+B*uold[M-1];
   for(int i = 1; i <= M-2; i++)
   {
      u[i]=A*uold[i-1]+B*uold[i]+C*uold[i+1];
   }

}

/*------------------------------------*/

void
apply_Aadjoint(double dt, double dx, double nu, int M, double *u)
{
   double A = -g(dt,dx)-b(dt,dx,nu);
   double B = 1+2*b(dt,dx,nu);
   double C = g(dt,dx)-b(dt,dx,nu);
   double *uold;
   vec_create(M, &uold);
   vec_copy(M, u, uold);
   u[0]=B*uold[0]+A*uold[1];
   u[M-1]=C*uold[M-2]+B*uold[M-1];
   for(int i = 1; i <= M-2; i++)
   {
      u[i]=C*uold[i-1]+B*uold[i]+A*uold[i+1];
   }

}

/*------------------------------------*/

void
apply_Uinv(double dt, double dx, int M, double *u)
{
   for (int i = 0; i <= M-1; i++)
    {
       u[i] /= dx*dt;
    }
}

/*------------------------------------*/

void
apply_Vinv(double dt, double dx, double alpha, int M, double *v)
{
   for (int i = 0; i <= M-1; i++)
   {
      v[i] /= alpha*dx*dt;
   }
   
}

/*------------------------------------*/

void
apply_D(double dt, double dx, double nu, int M, double *v, double *l, double *a)
{
   //add all arguments to apply_Phi below based on what Isaiah does
   /* apply_Phi(dt, dx, nu, M, v, l, a); */
    for (int i = 0; i <= M-1; i++)
    {
       v[i] *= dt;
    }
}

/*------------------------------------*/

void
apply_DAdjoint(double dt, double dx, double nu, int M, double *v, double *l, double *a)
{
   //add all arguments to apply_PhiAdjoing based on what Isaiah does
   /* apply_PhiAdjoint(dt, dx, nu, M, v, l, a); */
    for (int i = 0; i <= M-1; i++)
    {
       v[i] *= dt;
    }
}

/*------------------------------------*/

/*--------------------------------------------------------------------------
 * TriMGRIT wrapper routines
 *--------------------------------------------------------------------------*/


/*------------------------------------*/

/* This is only called from level 0 */

int
my_Init(braid_App     app,
        double        t,
        braid_Vector *u_ptr)
{
   my_Vector *u;
   int mspace = (app->mspace);

   /* Allocate the vector */
   u = (my_Vector *) malloc(sizeof(my_Vector));
   vec_create(mspace, &(u->values));

   for (int i = 0; i <= mspace-1; i++)
   {
      u->values[i] = ((double)braid_Rand())/braid_RAND_MAX;
   }

   *u_ptr = u;

   return 0;
}

/*------------------------------------*/

int
my_Clone(braid_App     app,
         braid_Vector  u,
         braid_Vector *v_ptr)
{
   int mspace = (app->mspace);
   my_Vector *v;

   /* Allocate the vector */
   v = (my_Vector *) malloc(sizeof(my_Vector));
   vec_create(mspace, &(v->values));

   /* Clone the values */
   for (int i = 0; i<= mspace-1; i++)
   {
      v->values[i] = u->values[i];
   }

   *v_ptr = v;

   return 0;
}

/*------------------------------------*/

int
my_Free(braid_App    app,
        braid_Vector u)
{
   free(u->values);
   free(u);

   return 0;
}

/*------------------------------------*/

int
my_Sum(braid_App     app,
       double        alpha,
       braid_Vector  x,
       double        beta,
       braid_Vector  y)
{
   int mspace = (app->mspace);
   for (int i = 0; i <= mspace-1; i++)
   {
      (y->values)[i] = alpha*(x->values)[i] + beta*(y->values)[i];
   }

   return 0;
}

/*------------------------------------*/

int
my_SpatialNorm(braid_App     app,
               braid_Vector  u,
               double       *norm_ptr)
{
   int i;
   double dot = 0.0;
   int mspace = (app->mspace);
   for (i = 0; i <= mspace-1; i++)
   {
      dot += (u->values)[i]*(u->values)[i];
   }
   *norm_ptr = sqrt(dot);

   return 0;
}

/*------------------------------------*/

// ZTODO: Need to compute u from adjoint and it reqires communication

int
my_Access(braid_App          app,
          braid_Vector       u,
          braid_AccessStatus astatus)
{
   int   done, index;
   int   mspace = (app->mspace);

   /* Print solution to file if simulation is over */
   braid_AccessStatusGetDone(astatus, &done);

   if (done)
   {
      /* Allocate w array in app (ZTODO: This only works on one proc right now) */
      if ((app->w) == NULL)
      {
         int  ntpoints;
         braid_AccessStatusGetNTPoints(astatus, &ntpoints);
         ntpoints++;  /* ntpoints is really the gupper index */
         (app->w) = (double **) calloc(ntpoints, sizeof(double *));
      }

      braid_AccessStatusGetTIndex(astatus, &index);
      if (app->w[index] != NULL)
      {
         free(app->w[index]);
      }
      vec_create(mspace, &(app->w[index]));
      vec_copy(mspace, (u->values), (app->w[index]));
   }
   return 0;
}

/*------------------------------------*/

int
my_BufSize(braid_App           app,
           int                 *size_ptr,
           braid_BufferStatus  bstatus)
{
   *size_ptr = 2*sizeof(double);
   return 0;
}

/*------------------------------------*/

int
my_BufPack(braid_App           app,
           braid_Vector        u,
           void               *buffer,
           braid_BufferStatus  bstatus)
{
   double *dbuffer = buffer;
   int i;
   int mspace = (app->mspace); 

   for(i = 0; i < mspace; i++)
   {
      dbuffer[i] = (u->values)[i];
   }

   braid_BufferStatusSetSize( bstatus,  2*sizeof(double));

   return 0;
}

/*------------------------------------*/

int
my_BufUnpack(braid_App           app,
             void               *buffer,
             braid_Vector       *u_ptr,
             braid_BufferStatus  bstatus)
{
   my_Vector *u = NULL;
   double    *dbuffer = buffer;
   int i;
   int mspace = (app->mspace); 

   /* Allocate memory */
   u = (my_Vector *) malloc(sizeof(my_Vector));
   u->values = (double*) malloc( 2*sizeof(double) );

   /* Unpack the buffer */
   for(i = 0; i < mspace; i++)
   {
      (u->values)[i] = dbuffer[i];
   }

   *u_ptr = u;
   return 0;
}

/*--------------------------------------------------------------------------
 * Main driver
 *--------------------------------------------------------------------------*/

int
main(int argc, char *argv[])
{
   my_App     *app;
         
   double      tstart, tstop, dt, dx, tol; 
   int         ntime, mspace, maxiter;
   double      alpha, nu, start, end, time, seed;
   /*
   int         max_levels, min_coarse, nrelax, nrelaxc, cfactor, maxiter;
   int         access_level, print_level;
   double      tol;
   */

   /* Define space domain. Space domain is between 0 and 1, mspace defines the number of steps */
   mspace = 12;
   ntime = 4096;

   /* Define some optimization parameters */
   alpha = .005;            /* parameter in the objective function */
   nu    = 1.5;                /* parameter in PDE */
   tol  = 1.0e-6;
   maxiter = 300;
   seed = 1.0;

   int arg_index = 1;
   while (arg_index < argc)
   {
      if ( strcmp(argv[arg_index], "-help") == 0 )
      {
         printf("\n");
         printf(" Solves the advection-diffusion model problem \n\n");
         printf("  min  1/2 \\int_0^T\\int_0^1 (u(x,t)-ubar(x))^2 + alpha*v(x,t)^2  dxdt \n\n");
         printf("  s.t.  u_t + u_x - nu*u_xx = v(x,t) \n");
         printf("        u(0,t) = u(1,t) = 0 \n\n");
         printf("        u(x,0) = u0(x) \n");
         printf("  -tstop <tstop>          : Upper integration limit for time\n");
         printf("  -ntime <ntime>          : Num points in time\n");
         printf("  -mspace <mspace>        : Num points in space\n");
         printf("  -nu <nu>                : Constant Parameter in PDE  \n");
         printf("  -alpha <alpha>          : Constant Parameter in Objective Function  \n");
         printf("  -ml <max_levels>        : Max number of braid levels \n");
         printf("  -mi <maxiter>           : Max iterations \n");
         printf("  -tol <tol>              : Stopping tolerance \n");
         printf("  -seed <seed>            : Seed for initial guess \n");
         exit(1);
      }
      else if ( strcmp(argv[arg_index], "-ntime") == 0 )
      {
         arg_index++;
         ntime = atoi(argv[arg_index++]);
      }
      else if ( strcmp(argv[arg_index], "-tstop") == 0 )
      {
         arg_index++;
         tstop = atoi(argv[arg_index++]);
      }
      else if ( strcmp(argv[arg_index], "-seed") == 0 )
      {
         arg_index++;
         seed = atoi(argv[arg_index++]);
      }      
      else if ( strcmp(argv[arg_index], "-mspace") == 0 )
      {
         arg_index++;
         mspace = atoi(argv[arg_index++]);
      }
      else if ( strcmp(argv[arg_index], "-nu") == 0 )
      {
         arg_index++;
         nu = atof(argv[arg_index++]);
      }
      else if ( strcmp(argv[arg_index], "-alpha") == 0 )
      {
         arg_index++;
         alpha = atof(argv[arg_index++]);
      }
      else if ( strcmp(argv[arg_index], "-mi") == 0 )
      {
         arg_index++;
         maxiter = atoi(argv[arg_index++]);
      }
      else if ( strcmp(argv[arg_index], "-tol") == 0 )
      {
         arg_index++;
         tol = atof(argv[arg_index++]);
      }
      else
      {
         printf("ABORTING: incorrect command line parameter %s\n", argv[arg_index]);
         return (0);
      }
   }

   printf("The rank max value is\n");
   printf("%f\n", (double)RAND_MAX);

   /* Define the space step */
   dx=(double)1/(mspace+1);

   /* Define time domain and step */
   tstart = 0.0;             /* Beginning of time domain */
   tstop  = 1.0;             /* End of time domain*/
   dt = (tstop-tstart)/ntime; 
    

   /* Set up the app structure */
   app = (my_App *) malloc(sizeof(my_App));
   app->ntime    = ntime;
   app->mspace   = mspace;
   app->nu       = nu;
   app->alpha    = alpha;
   app->w        = NULL;

   /* Set this to whatever u0 is. */
   double *U0 = (double*) malloc( mspace*sizeof(double) );
   double *u_init=(double*) malloc( mspace*sizeof(double) );
   
   for(int i=0; i<mspace/2; i++){
      U0[i]=1;
   }
   for(int i=mspace/2; i<mspace; i++)
   {
      U0[i]=0;
   }

   /* Set the initial guess to be random */
   srand(seed);
   for(int i=0; i<mspace; i++)
   {   
      double random_value;
      random_value=(double)rand()/RAND_MAX*2.0-1.0; 
      u_init[i]=random_value;
   }

   app->U0       = U0;

   /* Find elements of LU decomposition of A */

   double *ai = (double*) malloc( mspace*sizeof(double) );
   double *li = (double*) malloc( (mspace-1)*sizeof(double) );
   ai[0] = 1+2*b(dt,dx,nu);
   for(int i=1; i<mspace; i++){
      li[i-1] = -(b(dt,dx,nu)+g(dt,dx))/ai[i-1];
      ai[i] = ai[0]+(b(dt,dx,nu)-g(dt,dx))*li[i-1];
   }
   app->ai       = ai;
   app->li       = li;


   dx = 1/((double)(mspace+1));;
   

   /* Start the Gauss-Seidel iterations */
   start=clock();
   double norm=0;
   double niters=0;

   double **w = (double **)malloc(ntime * sizeof(double*));
   for(int i = 0; i < ntime; i++) w[i] = (double *)malloc(mspace * sizeof(double));
   
   double **v = (double **)malloc(ntime * sizeof(double*));
   for(int i = 0; i < ntime; i++) v[i] = (double *)malloc(mspace * sizeof(double));
   
   double **u = (double **)malloc(ntime * sizeof(double*));
   for(int i = 0; i < ntime; i++) u[i] = (double *)malloc(mspace * sizeof(double));
   
   double **res = (double **)malloc(ntime * sizeof(double*));
   for(int i = 0; i < ntime; i++) res[i] = (double *)malloc(mspace * sizeof(double));

   double **res1 = (double **)malloc(ntime * sizeof(double*));
   for(int i = 0; i < ntime; i++) res1[i] = (double *)malloc(mspace * sizeof(double));

   for(int i = 0; i < ntime; i++)
   {
      vec_copy(mspace, u_init, w[i]);
      vec_copy(mspace, u_init, v[i]);
      vec_copy(mspace, u_init, u[i]);      
      vec_copy(mspace, u_init, res[i]);
      vec_copy(mspace, u_init, res1[i]);
   }


   do
   {
      norm = 0;    
      /**************FORWARD SOLVE*******************/
      /* Solve Lu^(k+1)=g-Dv^(k) */
      vec_copy(mspace, v[0], u[0]);
      apply_D(dt, dx, nu, mspace, u[0], li, ai);
      /*vec_scale(mspace, -1.0, u[0]);*/
      vec_axpy(mspace, 1.0, U0, u[0]);
      apply_Phi(dt, dx, nu, mspace, u[0], li, ai);
      for (int i = 1; i < ntime; i++)
      {
         vec_copy(mspace, v[i], u[i]);
         apply_D(dt, dx, nu, mspace, u[i], li, ai);
         /*vec_scale(mspace, -1.0, u[i]);*/
         vec_axpy(mspace, 1.0, u[i-1], u[i]);
         apply_Phi(dt, dx, nu, mspace, u[i], li, ai);
      }

      /* Solve L*w^(k+1)=k-Uu^(k+1) */
      vec_copy(mspace, u[ntime-1], w[ntime-1]);
      vec_scale(mspace, -1.0, w[ntime-1]);
      vec_axpy(mspace, 1.0, U0, w[ntime-1]);
      vec_scale(mspace, dx*dt, w[ntime-1]); /* Apply U is the same as scaling by dxdt */
      apply_PhiAdjoint(dt, dx, nu, mspace, w[ntime-1], li, ai);
      for(int i = ntime-2; i >= 0; i--)
      {
         vec_copy(mspace, u[i], w[i]);
         vec_scale(mspace, -1.0, w[i]);
         vec_axpy(mspace, 1.0, U0, w[i]);
         vec_scale(mspace, dx*dt, w[i]); 
         vec_axpy(mspace, 1.0, w[i+1], w[i]);
         apply_PhiAdjoint(dt, dx, nu, mspace, w[i], li, ai);
      }

      /* Solve Vv^(k+1)=h-Dw^(k+1) */
      for(int i = 0; i < ntime; i++)
      {
         vec_copy(mspace, w[i], v[i]);
         apply_D(dt, dx, nu, mspace, v[i], li, ai);
         apply_Vinv(dt, dx, alpha, mspace, v[i]);
         /*vec_scale(mspace, -1.0, v[i]);*/
      }
      
      /****************************RESIDUAL*********************************/
      /* Compute residual for block Lu^(k+1)+Dv^(k+1)-g */
      vec_copy(mspace, u[0], res[0]);
      apply_A(dt, dx, nu, mspace, res[0]);
      
      vec_axpy(mspace, -1.0, U0, res[0]);
      
      vec_copy(mspace, v[0], res1[0]);
      vec_scale(mspace, -1.0*dt, res1[0]);
      vec_axpy(mspace, 1.0, res1[0], res[0]);
      for (int i = 1; i < ntime; i++)
      {
         vec_copy(mspace, u[i], res[i]);
         apply_A(dt, dx, nu, mspace, res[i]);
         
         vec_axpy(mspace, -1.0, u[i-1], res[i]);
         
         vec_copy(mspace, v[i], res1[i]);
         vec_scale(mspace, -1.0*dt, res1[i]);
         vec_axpy(mspace, 1.0, res1[i], res[i]);         
      }
      for(int i = 0; i < ntime; i++)
      {
         for (int j = 0; j < mspace; j++)
         {
            norm = norm + res[i][j]*res[i][j];
         }
      }

      /* Compute residual for block Uu^(k+1)+L*w^(k+1)-k */
      for(int i = 0; i < ntime-1; i++)
      {
         vec_copy(mspace, u[i], res[i]);
         vec_scale(mspace, dx*dt, res[i]);

         vec_copy(mspace, w[i], res1[i]);
         apply_Aadjoint(dt, dx, nu, mspace, res1[i]);
         vec_axpy(mspace, 1.0, res1[i], res[i]);

         vec_copy(mspace, w[i+1], res1[i]);
         vec_axpy(mspace, -1.0, res1[i], res[i]);

         vec_copy(mspace, U0, res1[i]);
         vec_scale(mspace, dx*dt, res1[i]);
         vec_axpy(mspace, -1.0, res1[i], res[i]);
      }
      vec_copy(mspace, u[ntime-1], res[ntime-1]);
      vec_scale(mspace, dx*dt, res[ntime-1]);

      vec_copy(mspace, w[ntime-1], res1[ntime-1]);
      apply_Aadjoint(dt, dx, nu, mspace, res1[ntime-1]);
      vec_axpy(mspace, 1.0, res1[ntime-1], res[ntime-1]);

      vec_copy(mspace, U0, res1[ntime-1]);
      vec_scale(mspace, dx*dt, res1[ntime-1]);
      vec_axpy(mspace, -1, res1[ntime-1], res[ntime-1]);
      for(int i = 0; i < ntime; i++)
      {
         for (int j = 0; j < mspace; j++)
         {
            norm = norm + res[i][j]*res[i][j];
         }
      }

      /* Compute residual for block D*w^(k+1)+Vv^(k+1)-0 */
      for(int i = 0; i< ntime-1; i++)
      {
         vec_copy(mspace, v[i], res[i]);
         vec_copy(mspace, w[i], res1[i]);
         vec_scale(mspace, alpha*dx*dt, res[i]);
         vec_scale(mspace, -1.0*dt, res1[i]);
         vec_axpy(mspace, 1.0, res1[i], res[i]);
      }          
      for(int i = 0; i < ntime; i++)
      {
         for (int j = 0; j < mspace; j++)
         {
            norm = norm + res[i][j]*res[i][j];
         }
      }
      norm = sqrt(norm);        
      /*****************************************/
      niters = niters + 1;
      printf("Residual: ");
      printf("%f", norm);
      printf("\n");
      printf("Iteration number: ");
      printf("%f", niters);
      printf("\n");
   }while(norm > tol && niters < maxiter);
   end=clock();
   time=(double)(end-start)/CLOCKS_PER_SEC;
   printf("The total run time is: ");
   printf("%f", time);
   printf(" seconds\n");

   /* Print out v, w, u and U0 */
   /**********************PRINT W OUT**********************/
   char  filename[255];
   FILE *file;
   int   i,j;

   sprintf(filename, "%s.%03d", "out/block_gs.out.w", 000);
   file = fopen(filename, "w");
   for (i = 0; i < (app->ntime); i++)
   {
      /* double **w = (app->w); */
      fprintf(file, "%05d: ", (i+1));
      for(j=0; j <mspace; j++){
         if(j==mspace-1){
            fprintf(file, "% 1.14e", w[i][j]);
         }
         else{
            fprintf(file, "% 1.14e, ", w[i][j]);
         }
      }
      fprintf(file, "\n");
   }
   fflush(file);
   fclose(file);

   /**********************PRINT V OUT**********************/
   sprintf(filename, "%s.%03d", "out/block_gs.out.v", 000);
   file = fopen(filename, "w");
   for (i = 0; i < (app->ntime); i++)
   {
      /* double **w = (app->w); */
      fprintf(file, "%05d: ", (i+1));
      for(j=0; j <mspace; j++){
         if(j==mspace-1){
            fprintf(file, "% 1.14e", v[i][j]);
         }
         else{
            fprintf(file, "% 1.14e, ", v[i][j]);
         }
      }
      fprintf(file, "\n");
   }
   fflush(file);
   fclose(file); 

   /**********************PRINT U OUT**********************/
   sprintf(filename, "%s.%03d", "out/block_gs.out.u", 000);
   file = fopen(filename, "w");
   for (i = 0; i < (app->ntime); i++)
   {
      /* double **w = (app->w); */
      fprintf(file, "%05d: ", (i+1));
      for(j=0; j <mspace; j++){
         if(j==mspace-1){
            fprintf(file, "% 1.14e", u[i][j]);
         }
         else{
            fprintf(file, "% 1.14e, ", u[i][j]);
         }
      }
      fprintf(file, "\n");
   }
   fflush(file);
   fclose(file);

   /**********************PRINT TIME OUT**********************/
   {
      sprintf(filename, "%s.%d", "out/block_gs.time", maxiter);
      file = fopen(filename, "w");
      fprintf(file, "%f", time);
      fflush(file);
      fclose(file);
   }

   /**********************PRINT TOTAL RES OUT**********************/
   {
      sprintf(filename, "%s.%d", "out/block_gs.res", maxiter);
      file = fopen(filename, "w");
      fprintf(file, "%f", norm);
      fflush(file);
      fclose(file);
   }

    /**********************PRINT OUT IF CONVERGE OR DIVERGE**********************/
   {
      sprintf(filename, "%s.%d.%f", "out/block_gs.conv", ntime, nu);
      file = fopen(filename, "w");
      if (isinf(norm)||isnan(norm))
      {
         fprintf(file, "%f", 0.0);
      }
      else
      {
         fprintf(file, "%f", 1.0);
      }
     
      fflush(file);
      fclose(file);
   } 

   /**********************PRINT U0 OUT**********************/
   char filename1[255];
   double *us;

   sprintf(filename1, "%s.%03d", "out/block_gs.u0", 000);
   file = fopen(filename1, "w");
   vec_create(mspace, &us);
   vec_copy(mspace, U0, us);
   for (j = 0; j < mspace; j++)
      {
         if(j!=mspace-1){
            fprintf(file, "% 1.14e, ", us[j]);
         }
         else{
            fprintf(file, "% 1.14e", us[j]);
         }
      }

   return (0);
}