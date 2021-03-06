/* 
 * simple lennard-jones potential MD code with velocity verlet.
 * units: Length=Angstrom, Mass=amu; Energy=kcal
 *
 * OpenCL parallel baseline version.
 * optimization 1: apply serial improvements except newtons 3rd law
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <math.h>

#include "OpenCL_utils.h"

#ifdef _USE_FLOAT
#define FPTYPE float
#define ZERO  0.0f
#define HALF  0.5f
#define TWO   2.0f
#define THREE 3.0f
static const char kernelflags[] = "-D_USE_FLOAT -cl-denorms-are-zero -cl-unsafe-math-optimizations";
#else
#define FPTYPE double
#define ZERO  0.0
#define HALF  0.5
#define TWO   2.0
#define THREE 3.0
static const char kernelflags[] = "-cl-unsafe-math-optimizations";
#endif

/* generic file- or pathname buffer length */
#define BLEN 200

/* a few physical constants */
const FPTYPE kboltz=0.0019872067;     /* boltzman constant in kcal/mol/K */
const FPTYPE mvsq2e=2390.05736153349; /* m*v^2 in kcal/mol */

/* structure to hold the complete information 
 * about the MD system */
struct _mdsys {
    int natoms,nfi,nsteps;
    FPTYPE dt, mass, epsilon, sigma, box, rcut;
    FPTYPE ekin, epot, temp;
    FPTYPE *rx, *ry, *rz;
    FPTYPE *vx, *vy, *vz;
    FPTYPE *fx, *fy, *fz;
};
typedef struct _mdsys mdsys_t;

/* structure to hold the complete information 
 * about the MD system on a OpenCL device*/
struct _cl_mdsys {
    int natoms,nfi,nsteps;
    FPTYPE dt, mass, epsilon, sigma, box, rcut;
    FPTYPE ekin, epot, temp;
    cl_mem rx, ry, rz;
    cl_mem vx, vy, vz;
    cl_mem fx, fy, fz;
};
typedef struct _cl_mdsys cl_mdsys_t;

/* helper function: read a line and then return
   the first string with whitespace stripped off */
static int get_me_a_line(FILE *fp, char *buf)
{
    char tmp[BLEN], *ptr;

    /* read a line and cut of comments and blanks */
    if (fgets(tmp,BLEN,fp)) {
        int i;

        ptr=strchr(tmp,'#');
        if (ptr) *ptr= '\0';
        i=strlen(tmp); --i;
        while(isspace(tmp[i])) {
            tmp[i]='\0';
            --i;
        }
        ptr=tmp;
        while(isspace(*ptr)) {++ptr;}
        i=strlen(ptr);
        strcpy(buf,tmp);
        return 0;
    } else {
        perror("problem reading input");
        return -1;
    }
    return 0;
}
 
void PrintUsageAndExit() {
    fprintf( stderr, "\nError. Run the program as follow: ");
    fprintf( stderr, "\n./ljmd-cl.x device [thread-number] < input ");
    fprintf( stderr, "\ndevice = cpu | gpu \n\n" );
    exit(1);
}

/* append data to output. */
static void output(mdsys_t *sys, FILE *erg, FILE *traj)
{
    int i;
    
    printf("% 8d % 20.8f % 20.8f % 20.8f % 20.8f\n", sys->nfi, sys->temp, sys->ekin, sys->epot, sys->ekin+sys->epot);
    fprintf(erg,"% 8d % 20.8f % 20.8f % 20.8f % 20.8f\n", sys->nfi, sys->temp, sys->ekin, sys->epot, sys->ekin+sys->epot);
    fprintf(traj,"%d\n nfi=%d etot=%20.8f\n", sys->natoms, sys->nfi, sys->ekin+sys->epot);
    for (i=0; i<sys->natoms; ++i) {
      fprintf(traj, "Ar  %20.8f %20.8f %20.8f\n", sys->rx[i], sys->ry[i], sys->rz[i]);
    }
}





/* main */
int main(int argc, char **argv) 
{
  /*OpenCL variables */
  cl_device_id device;
  cl_device_type device_type; /*to test if we are on cpu or gpu*/
  cl_context context;
  cl_command_queue cmdQueue;

  FPTYPE * buffers[3];
  cl_mdsys_t cl_sys;
  cl_int status;

  int nprint, i, nthreads = 0;
  char restfile[BLEN], trajfile[BLEN], ergfile[BLEN], line[BLEN];
  FILE *fp,*traj,*erg;
  mdsys_t sys;


/* Start profiling */

#ifdef __PROFILING
  
  double t1, t2;

  t1 = second();

#endif

  /* handling the command line arguments */
  switch (argc) {
      case 2: /* only the cpu/gpu argument was passed, setting default nthreads */
	      if( !strcmp( argv[1], "cpu" ) ) nthreads = 16;
	      else nthreads = 1024;
	      break;
      case 3: /* both the device type (cpu/gpu) and the number of threads were passed */
	      nthreads = strtol(argv[2],NULL,10);
	      if( nthreads<0 ) {
		      fprintf( stderr, "\n. The number of threads must be more than 1.\n");
		      PrintUsageAndExit();
	      }
	      break;
      default:
	      PrintUsageAndExit();
	      break;
  }
  
  /* Initialize the OpenCL environment */
  if( InitOpenCLEnvironment( argv[1], &device, &context, &cmdQueue ) != CL_SUCCESS ){
    fprintf( stderr, "Program Error! OpenCL Environment was not initialized correctly.\n" );
    return 4;
  }

  /* read input file */
  if(get_me_a_line(stdin,line)) return 1;
  sys.natoms=atoi(line);
  if(get_me_a_line(stdin,line)) return 1;
  sys.mass=atof(line);
  if(get_me_a_line(stdin,line)) return 1;
  sys.epsilon=atof(line);
  if(get_me_a_line(stdin,line)) return 1;
  sys.sigma=atof(line);
  if(get_me_a_line(stdin,line)) return 1;
  sys.rcut=atof(line);
  if(get_me_a_line(stdin,line)) return 1;
  sys.box=atof(line);
  if(get_me_a_line(stdin,restfile)) return 1;
  if(get_me_a_line(stdin,trajfile)) return 1;
  if(get_me_a_line(stdin,ergfile)) return 1;
  if(get_me_a_line(stdin,line)) return 1;
  sys.nsteps=atoi(line);
  if(get_me_a_line(stdin,line)) return 1;
  sys.dt=atof(line);
  if(get_me_a_line(stdin,line)) return 1;
  nprint=atoi(line);
  

  
  /* allocate memory */
  cl_sys.natoms = sys.natoms;
  cl_sys.rx = clCreateBuffer( context, CL_MEM_READ_WRITE, cl_sys.natoms * sizeof(FPTYPE), NULL, &status );
  cl_sys.ry = clCreateBuffer( context, CL_MEM_READ_WRITE, cl_sys.natoms * sizeof(FPTYPE), NULL, &status );
  cl_sys.rz = clCreateBuffer( context, CL_MEM_READ_WRITE, cl_sys.natoms * sizeof(FPTYPE), NULL, &status );
  cl_sys.vx = clCreateBuffer( context, CL_MEM_READ_WRITE, cl_sys.natoms * sizeof(FPTYPE), NULL, &status );
  cl_sys.vy = clCreateBuffer( context, CL_MEM_READ_WRITE, cl_sys.natoms * sizeof(FPTYPE), NULL, &status );
  cl_sys.vz = clCreateBuffer( context, CL_MEM_READ_WRITE, cl_sys.natoms * sizeof(FPTYPE), NULL, &status );
  cl_sys.fx = clCreateBuffer( context, CL_MEM_READ_WRITE, cl_sys.natoms * sizeof(FPTYPE), NULL, &status );
  cl_sys.fy = clCreateBuffer( context, CL_MEM_READ_WRITE, cl_sys.natoms * sizeof(FPTYPE), NULL, &status );
  cl_sys.fz = clCreateBuffer( context, CL_MEM_READ_WRITE, cl_sys.natoms * sizeof(FPTYPE), NULL, &status );
  
  buffers[0] = (FPTYPE *) malloc( 2 * cl_sys.natoms * sizeof(FPTYPE) );
  buffers[1] = (FPTYPE *) malloc( 2 * cl_sys.natoms * sizeof(FPTYPE) );
  buffers[2] = (FPTYPE *) malloc( 2 * cl_sys.natoms * sizeof(FPTYPE) );
  
  /* read restart */
  fp = fopen( restfile, "r" );
  if( fp ) {
    for( i = 0; i < 2 * cl_sys.natoms; ++i ){
#ifdef _USE_FLOAT
      fscanf( fp, "%f%f%f", buffers[0] + i, buffers[1] + i, buffers[2] + i);
#else
      fscanf( fp, "%lf%lf%lf", buffers[0] + i, buffers[1] + i, buffers[2] + i);
#endif
    }
    
    status = clEnqueueWriteBuffer( cmdQueue, cl_sys.rx, CL_TRUE, 0, cl_sys.natoms * sizeof(FPTYPE), buffers[0], 0, NULL, NULL ); 
    status |= clEnqueueWriteBuffer( cmdQueue, cl_sys.ry, CL_TRUE, 0, cl_sys.natoms * sizeof(FPTYPE), buffers[1], 0, NULL, NULL ); 
    status |= clEnqueueWriteBuffer( cmdQueue, cl_sys.rz, CL_TRUE, 0, cl_sys.natoms * sizeof(FPTYPE), buffers[2], 0, NULL, NULL ); 
    
    status |= clEnqueueWriteBuffer( cmdQueue, cl_sys.vx, CL_TRUE, 0, cl_sys.natoms * sizeof(FPTYPE), buffers[0] + cl_sys.natoms, 0, NULL, NULL ); 
    status |= clEnqueueWriteBuffer( cmdQueue, cl_sys.vy, CL_TRUE, 0, cl_sys.natoms * sizeof(FPTYPE), buffers[1] + cl_sys.natoms, 0, NULL, NULL ); 
    status |= clEnqueueWriteBuffer( cmdQueue, cl_sys.vz, CL_TRUE, 0, cl_sys.natoms * sizeof(FPTYPE), buffers[2] + cl_sys.natoms, 0, NULL, NULL ); 
    
    fclose(fp);

  } else {
    perror("cannot read restart file");
    return 3;
  }
  
  /* initialize forces and energies.*/
  sys.nfi=0;
  
  size_t globalWorkSize[1];
  globalWorkSize[0] = nthreads;
  
  const char * sourcecode =
  #include <opencl_kernels_as_string.h>
  ;

  cl_program program = clCreateProgramWithSource( context, 1, (const char **) &sourcecode, NULL, &status );
  
  status |= clBuildProgram( program, 0, NULL, kernelflags, NULL, NULL );
  
#ifdef __DEBUG
  size_t log_size;
  char log [200000]; 
  clGetProgramBuildInfo( program, device, CL_PROGRAM_BUILD_LOG, sizeof(log), log, &log_size );
  fprintf( stderr, "\nLog: \n\n %s", log ); 
#endif
  
  cl_kernel kernel_force = clCreateKernel( program, "opencl_force", &status );
  cl_kernel kernel_ekin = clCreateKernel( program, "opencl_ekin", &status );
  cl_kernel kernel_verlet_first = clCreateKernel( program, "opencl_verlet_first", &status );
  cl_kernel kernel_verlet_second = clCreateKernel( program, "opencl_verlet_second", &status );
  cl_kernel kernel_azzero = clCreateKernel( program, "opencl_azzero", &status );
  
  FPTYPE * tmp_epot;
  cl_mem epot_buffer;
  tmp_epot = (FPTYPE *) malloc( nthreads * sizeof(FPTYPE) );
  epot_buffer = clCreateBuffer( context, CL_MEM_READ_WRITE, nthreads * sizeof(FPTYPE), NULL, &status );
  
  /* precompute some constants */
  FPTYPE c12 = 4.0 * sys.epsilon * pow( sys.sigma, 12.0);
  FPTYPE c6  = 4.0 * sys.epsilon * pow( sys.sigma, 6.0);
  FPTYPE rcsq = sys.rcut * sys.rcut;
  FPTYPE boxby2 = HALF * sys.box;  
  FPTYPE dtmf = HALF * sys.dt / mvsq2e / sys.mass;
  sys.epot = ZERO;
  sys.ekin = ZERO;

  /* Azzero force buffer */
  status = clSetMultKernelArgs( kernel_azzero, 0, 4, KArg(cl_sys.fx), KArg(cl_sys.fy), KArg(cl_sys.fz), KArg(cl_sys.natoms));

  status = clEnqueueNDRangeKernel( cmdQueue, kernel_azzero, 1, NULL, globalWorkSize, NULL, 0, NULL, NULL );

  status |= clSetMultKernelArgs( kernel_force, 0, 13,
	KArg(cl_sys.fx),
	KArg(cl_sys.fy),
	KArg(cl_sys.fz),
	KArg(cl_sys.rx),
	KArg(cl_sys.ry),
	KArg(cl_sys.rz),
	KArg(cl_sys.natoms),
	KArg(epot_buffer),
	KArg(c12),
	KArg(c6),
	KArg(rcsq),
	KArg(boxby2),
	KArg(sys.box));
  
  status = clEnqueueNDRangeKernel( cmdQueue, kernel_force, 1, NULL, globalWorkSize, NULL, 0, NULL, NULL );
  
  status |= clEnqueueReadBuffer( cmdQueue, epot_buffer, CL_TRUE, 0, nthreads * sizeof(FPTYPE), tmp_epot, 0, NULL, NULL );     
  
  for( i = 0; i < nthreads; i++) sys.epot += tmp_epot[i];
  
  FPTYPE * tmp_ekin;
  cl_mem ekin_buffer;
  tmp_ekin = (FPTYPE *) malloc( nthreads * sizeof(FPTYPE) );
  ekin_buffer = clCreateBuffer( context, CL_MEM_READ_WRITE, nthreads * sizeof(FPTYPE), NULL, &status );
  
  status |= clSetMultKernelArgs( kernel_ekin, 0, 5, KArg(cl_sys.vx), KArg(cl_sys.vy), KArg(cl_sys.vz),
    KArg(cl_sys.natoms), KArg(ekin_buffer));
  
  status = clEnqueueNDRangeKernel( cmdQueue, kernel_ekin, 1, NULL, globalWorkSize, NULL, 0, NULL, NULL );
    
  status |= clEnqueueReadBuffer( cmdQueue, ekin_buffer, CL_TRUE, 0, nthreads * sizeof(FPTYPE), tmp_ekin, 0, NULL, NULL );     

  for( i = 0; i < nthreads; i++) sys.ekin += tmp_ekin[i];
  sys.ekin *= HALF * mvsq2e * sys.mass;
  sys.temp  = TWO * sys.ekin / ( THREE * sys.natoms - THREE ) / kboltz;

  erg=fopen(ergfile,"w");
  traj=fopen(trajfile,"w");

  printf("Starting simulation with %d atoms for %d steps.\n",sys.natoms, sys.nsteps);
  printf("     NFI            TEMP            EKIN                 EPOT              ETOT\n");
  
  /* download data on host */
  status = clEnqueueReadBuffer( cmdQueue, cl_sys.rx, CL_TRUE, 0, cl_sys.natoms * sizeof(FPTYPE), buffers[0], 0, NULL, NULL ); 
  status |= clEnqueueReadBuffer( cmdQueue, cl_sys.ry, CL_TRUE, 0, cl_sys.natoms * sizeof(FPTYPE), buffers[1], 0, NULL, NULL ); 
  status |= clEnqueueReadBuffer( cmdQueue, cl_sys.rz, CL_TRUE, 0, cl_sys.natoms * sizeof(FPTYPE), buffers[2], 0, NULL, NULL ); 
  
  sys.rx = buffers[0];
  sys.ry = buffers[1];
  sys.rz = buffers[2];
  
  output(&sys, erg, traj);

  /**************************************************/
  /* main MD loop */
  for(sys.nfi=1; sys.nfi <= sys.nsteps; ++sys.nfi) {

    /* This is a placeholder for the barrier that will be needed
     * once the ReadBuffer & WriteBuffer calls are transformed to
     * non blocking ones */

    /*
     * if ((sys.nfi % nprint) == 0) BARRIER(event[8]);
     */


    /* propagate system and recompute energies */
    /* 2) verlet_first   */
    status |= clSetMultKernelArgs( kernel_verlet_first, 0, 12,
      KArg(cl_sys.fx),
      KArg(cl_sys.fy),
      KArg(cl_sys.fz),
      KArg(cl_sys.rx),
      KArg(cl_sys.ry),
      KArg(cl_sys.rz),
      KArg(cl_sys.vx),
      KArg(cl_sys.vy),
      KArg(cl_sys.vz),
      KArg(cl_sys.natoms),
      KArg(sys.dt),
      KArg(dtmf));

    CheckSuccess(status, 2);
    status = clEnqueueNDRangeKernel( cmdQueue, kernel_verlet_first, 1, NULL, globalWorkSize, NULL, 0, NULL, NULL );

    /* 6) download position@device to position@host */
    if ((sys.nfi % nprint) == nprint-1) {
	status = clEnqueueReadBuffer( cmdQueue, cl_sys.rx, CL_TRUE, 0, cl_sys.natoms * sizeof(FPTYPE), buffers[0], 0, NULL, NULL );
	status |= clEnqueueReadBuffer( cmdQueue, cl_sys.ry, CL_TRUE, 0, cl_sys.natoms * sizeof(FPTYPE), buffers[1], 0, NULL, NULL );
	status |= clEnqueueReadBuffer( cmdQueue, cl_sys.rz, CL_TRUE, 0, cl_sys.natoms * sizeof(FPTYPE), buffers[2], 0, NULL, NULL );

	CheckSuccess(status, 6);
	sys.rx = buffers[0];
	sys.ry = buffers[1];
	sys.rz = buffers[2];
    }

    /* 3) force */
    status |= clSetMultKernelArgs( kernel_force, 0, 13,
      KArg(cl_sys.fx),
      KArg(cl_sys.fy),
      KArg(cl_sys.fz),
      KArg(cl_sys.rx),
      KArg(cl_sys.ry),
      KArg(cl_sys.rz),
      KArg(cl_sys.natoms),
      KArg(epot_buffer),
      KArg(c12),
      KArg(c6),
      KArg(rcsq),
      KArg(boxby2),
      KArg(sys.box));

    CheckSuccess(status, 3);
    status = clEnqueueNDRangeKernel( cmdQueue, kernel_force, 1, NULL, globalWorkSize, NULL, 0, NULL, NULL );

    /* 7) download E_pot[i]@device and perform reduction to E_pot@host */
    if ((sys.nfi % nprint) == nprint-1) {
	status |= clEnqueueReadBuffer( cmdQueue, epot_buffer, CL_TRUE, 0, nthreads * sizeof(FPTYPE), tmp_epot, 0, NULL, NULL );
	CheckSuccess(status, 7);
    }

    /* 4) verlet_second */
    status |= clSetMultKernelArgs( kernel_verlet_second, 0, 9,
      KArg(cl_sys.fx),
      KArg(cl_sys.fy),
      KArg(cl_sys.fz),
      KArg(cl_sys.vx),
      KArg(cl_sys.vy),
      KArg(cl_sys.vz),
      KArg(cl_sys.natoms),
      KArg(sys.dt),
      KArg(dtmf));

    CheckSuccess(status, 4);
    status = clEnqueueNDRangeKernel( cmdQueue, kernel_verlet_second, 1, NULL, globalWorkSize, NULL, 0, NULL, NULL );

    if ((sys.nfi % nprint) == nprint-1) {

	/* 5) ekin */
	status |= clSetMultKernelArgs( kernel_ekin, 0, 5, KArg(cl_sys.vx), KArg(cl_sys.vy), KArg(cl_sys.vz),
			KArg(cl_sys.natoms), KArg(ekin_buffer));
	CheckSuccess(status, 5);
	status = clEnqueueNDRangeKernel( cmdQueue, kernel_ekin, 1, NULL, globalWorkSize, NULL, 0, NULL, NULL );


	/* 8) download E_kin[i]@device and perform reduction to E_kin@host */
	status |= clEnqueueReadBuffer( cmdQueue, ekin_buffer, CL_TRUE, 0, nthreads * sizeof(FPTYPE), tmp_ekin, 0, NULL, NULL );
	CheckSuccess(status, 8);
    }

    /* 1) write output every nprint steps */
    if ((sys.nfi % nprint) == 0) {

	/* initialize the sys.epot@host and sys.ekin@host variables to ZERO */
	sys.epot = ZERO;
	sys.ekin = ZERO;

	/* reduction on the tmp_Exxx[i] buffers downloaded from the device
	 * during parts 7 and 8 of the previous MD loop iteration */
	for( i = 0; i < nthreads; i++) {
		sys.epot += tmp_epot[i];
		sys.ekin += tmp_ekin[i];
	}

	/* multiplying the kinetic energy by prefactors */
	sys.ekin *= HALF * mvsq2e * sys.mass;
	sys.temp  = TWO * sys.ekin / ( THREE * sys.natoms - THREE ) / kboltz;

	/* writing output files (positions, energies and temperature) */
	output(&sys, erg, traj);
    }

  }
  /**************************************************/

/* End profiling */

#ifdef __PROFILING

t2 = second();

fprintf( stdout, "\n\nTime of execution = %.3g (seconds)\n", (t2 - t1) );

#endif






  /* clean up: close files, free memory */
  printf("Simulation Done.\n");
  fclose(erg);
  fclose(traj);

  free(buffers[0]);
  free(buffers[1]);
  free(buffers[2]);

  return 0;
}
