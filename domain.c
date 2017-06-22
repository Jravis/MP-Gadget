#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <math.h>


#include "allvars.h"
#include "proto.h"
#include "forcetree.h"
#include "mymalloc.h"
#include "mpsort.h"
#include "endrun.h"
#include "openmpsort.h"
#include "domain.h"
#include "timestep.h"
#include "system.h"

#define TAG_GRAV_A        18
#define TAG_GRAV_B        19

/*! \file domain.c
 *  \brief code for domain decomposition
 *
 *  This file contains the code for the domain decomposition of the
 *  simulation volume.  The domains are constructed from disjoint subsets
 *  of the leaves of a fiducial top-level tree that covers the full
 *  simulation volume. Domain boundaries hence run along tree-node
 *  divisions of a fiducial global BH tree. As a result of this method, the
 *  tree force are in principle strictly independent of the way the domains
 *  are cut. The domain decomposition can be carried out for an arbitrary
 *  number of CPUs. Individual domains are not cubical, but spatially
 *  coherent since the leaves are traversed in a Peano-Hilbert order and
 *  individual domains form segments along this order.  This also ensures
 *  that each domain has a small surface to volume ratio, which minimizes
 *  communication.
 */

/*Only used in forcetree.c*/
int *DomainStartList, *DomainEndList;

int *DomainTask;

struct topnode_data *TopNodes;

int NTopnodes, NTopleaves;


static struct local_topnode_data
{
    peanokey Size;		/*!< number of Peano-Hilbert mesh-cells represented by top-level node */
    peanokey StartKey;		/*!< first Peano-Hilbert key in top-level node */
    int64_t Count;		/*!< counts the number of particles in this top-level node */
    int Daughter;			/*!< index of first daughter cell (out of 8) of top-level node */
    int Leaf;			/*!< if the node is a leaf, this gives its number when all leaves are traversed in Peano-Hilbert order */
    /*Below members are only used in this file*/
    int Parent;
    int PIndex;			/*!< first particle in node  used only in top-level tree build (this file)*/
    double Cost;
}
*topNodes;			/*!< points to the root node of the top-level tree */


static void domain_findSplit_work_balanced(int ncpu, int ndomain, float *domainWork);
static void domain_findSplit_load_balanced(int ncpu, int ndomain, int *domainCount);
static void domain_assign_balanced(float* domainWork, int* domainCount);
static void domain_allocate(void);
int domain_check_memory_bound(const int print_details, float *domainWork, int *domainCount);
static int domain_decompose(void);
static int domain_determineTopTree(void);
static void domain_free(void);
static void domain_sumCost(float *domainWork, int *domainCount);

static void domain_insertnode(struct local_topnode_data *treeA, struct local_topnode_data *treeB, int noA,
        int noB);
static void domain_add_cost(struct local_topnode_data *treeA, int noA, int64_t count, double cost);
int domain_check_for_local_refine(const int i, const struct peano_hilbert_data * mp);

static int domain_layoutfunc(int n);
static int domain_countToGo(ptrdiff_t nlimit, int (*layoutfunc)(int p), int* toGo, int * toGoSph, int * toGoBh, int *toGet, int *toGetSph, int *toGetBh);
static void domain_exchange_once(int (*layoutfunc)(int p), int* toGo, int * toGoSph, int * toGoBh, int *toGet, int *toGetSph, int *toGetBh);

static int domain_allocated_flag = 0;

static MPI_Datatype MPI_TYPE_PARTICLE = 0;
static MPI_Datatype MPI_TYPE_SPHPARTICLE = 0;
static MPI_Datatype MPI_TYPE_BHPARTICLE = 0;

static int
domain_all_garbage_collection();
static int
domain_sph_garbage_collection_reclaim();
static int
domain_bh_garbage_collection();

/*! This is the main routine for the domain decomposition.  It acts as a
 *  driver routine that allocates various temporary buffers, maps the
 *  particles back onto the periodic box if needed, and then does the
 *  domain decomposition, and a final Peano-Hilbert order of all particles
 *  as a tuning measure.
 */
void domain_Decomposition(void)
{
    int retsum;
    double t0, t1;

    /* register the mpi types used in communication if not yet. */
    if (MPI_TYPE_PARTICLE == 0) {
        MPI_Type_contiguous(sizeof(struct particle_data), MPI_BYTE, &MPI_TYPE_PARTICLE);
        MPI_Type_contiguous(sizeof(struct bh_particle_data), MPI_BYTE, &MPI_TYPE_BHPARTICLE);
        MPI_Type_contiguous(sizeof(struct sph_particle_data), MPI_BYTE, &MPI_TYPE_SPHPARTICLE);
        MPI_Type_commit(&MPI_TYPE_PARTICLE);
        MPI_Type_commit(&MPI_TYPE_BHPARTICLE);
        MPI_Type_commit(&MPI_TYPE_SPHPARTICLE);
    }

    walltime_measure("/Misc");

    move_particles(All.Ti_Current);

    if(force_tree_allocated()) force_tree_free();

    domain_garbage_collection();

    domain_free();

    do_box_wrapping();	/* map the particles back onto the box */

    message(0, "domain decomposition... (presently allocated=%g MB)\n", AllocatedBytes / (1024.0 * 1024.0));

    t0 = second();

    do
    {
        int ret;
#ifdef DEBUG
        message(0, "Testing ID Uniqueness before domain decompose\n");
        test_id_uniqueness();
#endif
        domain_allocate();
        ret = domain_decompose();

        MPI_Allreduce(&ret, &retsum, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
        if(retsum)
        {
            domain_free();
            message(0, "Increasing TopNodeAllocFactor=%g  ", All.TopNodeAllocFactor);

            All.TopNodeAllocFactor *= 1.3;

            message(0, "new value=%g\n", All.TopNodeAllocFactor);

            if(All.TopNodeAllocFactor > 1000)
            {
                if(ThisTask == 0)
                    endrun(781, "something seems to be going seriously wrong here. Stopping.\n");
            }
        }
    }
    while(retsum);

    t1 = second();

    message(0, "domain decomposition done. (took %g sec)\n", timediff(t0, t1));

    peano_hilbert_order();

    walltime_measure("/Domain/Peano");

    memmove(TopNodes + NTopnodes, DomainTask, NTopnodes * sizeof(int));

    TopNodes = (struct topnode_data *) myrealloc(TopNodes,
            (NTopnodes * sizeof(struct topnode_data) + NTopnodes * sizeof(int)));
    message(0, "Freed %g MByte in top-level domain structure\n",
                (MaxTopNodes - NTopnodes) * sizeof(struct topnode_data) / (1024.0 * 1024.0));

    DomainTask = (int *) (TopNodes + NTopnodes);

    reconstruct_timebins();
    walltime_measure("/Domain/Misc");
}

/*! This function allocates all the stuff that will be required for the tree-construction/walk later on */
void domain_allocate(void)
{
    size_t bytes, all_bytes = 0;

    MaxTopNodes = (int) (All.TopNodeAllocFactor * All.MaxPart + 1);

    DomainStartList = (int *) mymalloc("DomainStartList", bytes = (NTask * All.DomainOverDecompositionFactor * sizeof(int)));
    all_bytes += bytes;

    DomainEndList = (int *) mymalloc("DomainEndList", bytes = (NTask * All.DomainOverDecompositionFactor * sizeof(int)));
    all_bytes += bytes;

    TopNodes = (struct topnode_data *) mymalloc("TopNodes", bytes =
            (MaxTopNodes * sizeof(struct topnode_data) +
             MaxTopNodes * sizeof(int)));
    all_bytes += bytes;

    DomainTask = (int *) (TopNodes + MaxTopNodes);

    message(0, "Allocated %g MByte for top-level domain structure\n", all_bytes / (1024.0 * 1024.0));

    domain_allocated_flag = 1;
}

void domain_free(void)
{
    if(domain_allocated_flag)
    {
        myfree(TopNodes);
        myfree(DomainEndList);
        myfree(DomainStartList);
        domain_allocated_flag = 0;
    }
}

float domain_particle_costfactor(int i)
{
    if(P[i].TimeBin)
        return (1.0 + P[i].GravCost) / (1 << P[i].TimeBin);
    else
        return (1.0 + P[i].GravCost) / TIMEBASE;
}

static void
domain_count_particles()
{
    int i;
    for(i = 0; i < 6; i++)
        NLocal[i] = 0;

#pragma omp parallel private(i)
    {
        int NLocalThread[6] = {0};
#pragma omp for
        for(i = 0; i < NumPart; i++)
        {
            NLocalThread[P[i].Type]++;
        }
#pragma omp critical 
        {
/* avoid omp reduction for now: Craycc doesn't always do it right */
            for(i = 0; i < 6; i ++) {
                NLocal[i] += NLocalThread[i];
            }
        }
    }
    domain_refresh_totals();
}

/*! This function carries out the actual domain decomposition for all
 *  particle types. It will try to balance the work-load for each domain,
 *  as estimated based on the P[i]-GravCost values.  The decomposition will
 *  respect the maximum allowed memory-imbalance given by the value of
 *  PartAllocFactor.
 */
int domain_decompose(void)
{

    int i, status;

    size_t bytes, all_bytes = 0;

    /*!< a table that gives the total "work" due to the particles stored by each processor */
    float *domainWork = (float *) mymalloc("domainWork", bytes = (MaxTopNodes * sizeof(float)));
    all_bytes += bytes;
    /*!< a table that gives the total number of particles held by each processor */
    int * domainCount = (int *) mymalloc("domainCount", bytes = (MaxTopNodes * sizeof(int)));
    all_bytes += bytes;

    topNodes = (struct local_topnode_data *) mymalloc("topNodes", bytes =
            (MaxTopNodes *
             sizeof(struct local_topnode_data)));
    memset(topNodes, 0, sizeof(topNodes[0]) * MaxTopNodes);
    all_bytes += bytes;

    message(0, "use of %g MB of temporary storage for domain decomposition... (presently allocated=%g MB)\n",
             all_bytes / (1024.0 * 1024.0), AllocatedBytes / (1024.0 * 1024.0));

    report_memory_usage("DOMAIN");

    walltime_measure("/Domain/Decompose/Misc");

    /*Make an array of peano keys so we don't have to recompute them inside the domain*/
    #pragma omp parallel for
    for(i=0; i<NumPart; i++)
        P[i].Key = KEY(i);

    if(domain_determineTopTree())
        return 1;
    /* count toplevel leaves */
    domain_sumCost(domainWork, domainCount);
    walltime_measure("/Domain/DetermineTopTree/Sumcost");

    if(NTopleaves < All.DomainOverDecompositionFactor * NTask)
        endrun(112, "Number of Topleaves is less than required over decomposition");


    /* find the split of the domain grid */
    domain_findSplit_work_balanced(All.DomainOverDecompositionFactor * NTask, NTopleaves,domainWork);

    walltime_measure("/Domain/Decompose/findworksplit");

    domain_assign_balanced(domainWork, NULL);

    walltime_measure("/Domain/Decompose/assignbalance");

    status = domain_check_memory_bound(0,domainWork,domainCount);
    walltime_measure("/Domain/Decompose/memorybound");

    if(status != 0)		/* the optimum balanced solution violates memory constraint, let's try something different */
    {
        message(0, "Note: the domain decomposition is suboptimum because the ceiling for memory-imbalance is reached\n");

        domain_findSplit_load_balanced(All.DomainOverDecompositionFactor * NTask, NTopleaves,domainCount);

        walltime_measure("/Domain/Decompose/findloadsplit");
        domain_assign_balanced(NULL, domainCount);
        walltime_measure("/Domain/Decompose/assignbalance");

        status = domain_check_memory_bound(1,domainWork,domainCount);
        walltime_measure("/Domain/Decompose/memorybound");

        if(status != 0)
        {
            endrun(0, "No domain decomposition that stays within memory bounds is possible.\n");
        }
    }

    walltime_measure("/Domain/Decompose/Misc");
    domain_exchange(domain_layoutfunc);

    /* copy what we need for the topnodes */
    for(i = 0; i < NTopnodes; i++)
    {
        TopNodes[i].StartKey = topNodes[i].StartKey;
        TopNodes[i].Size = topNodes[i].Size;
        TopNodes[i].Daughter = topNodes[i].Daughter;
        TopNodes[i].Leaf = topNodes[i].Leaf;
    }

    myfree(topNodes);
    myfree(domainCount);
    myfree(domainWork);

    return 0;
}

void checklock() {
    int j;
#ifdef OPENMP_USE_SPINLOCK
    for(j = 0; j < All.MaxPart; j++) {
        if(0 == P[j].SpinLock) {
            endrun(1, "lock failed %d, %d\n", j, P[j].SpinLock);
        }
    }
#endif
}
/* 
 * 
 * exchange particles according to layoutfunc.
 * layoutfunc gives the target task of particle p.
*/

void domain_exchange(int (*layoutfunc)(int p)) {
    int i;
    int64_t sumtogo;
    /*! toGo[task*NTask + partner] gives the number of particles in task 'task'
     *  that have to go to task 'partner'
     */
    /* flag the particles that need to be exported */
    int * toGo = (int *) mymalloc("toGo", (sizeof(int) * NTask));
    int * toGoSph = (int *) mymalloc("toGoSph", (sizeof(int) * NTask));
    int * toGoBh = (int *) mymalloc("toGoBh", (sizeof(int) * NTask));
    int * toGet = (int *) mymalloc("toGet", (sizeof(int) * NTask));
    int * toGetSph = (int *) mymalloc("toGetSph", (sizeof(int) * NTask));
    int * toGetBh = (int *) mymalloc("toGetBh", (sizeof(int) * NTask));


#pragma omp parallel for
    for(i = 0; i < NumPart; i++)
    {
        int target = layoutfunc(i);
        if(target != ThisTask)
            P[i].OnAnotherDomain = 1;
        P[i].WillExport = 0;
    }

    walltime_measure("/Domain/exchange/init");

    int iter = 0, ret;
    ptrdiff_t exchange_limit;

    do
    {
        exchange_limit = FreeBytes - NTask * (24 * sizeof(int) + 16 * sizeof(MPI_Request));

        if(exchange_limit <= 0)
        {
            endrun(1, "exchange_limit=%d < 0\n", (int) exchange_limit);
        }

        /* determine for each cpu how many particles have to be shifted to other cpus */
        ret = domain_countToGo(exchange_limit, layoutfunc, toGo, toGoSph, toGoBh,toGet, toGetSph, toGetBh);
        walltime_measure("/Domain/exchange/togo");

        for(i = 0, sumtogo = 0; i < NTask; i++)
            sumtogo += toGo[i];

        sumup_longs(1, &sumtogo, &sumtogo);

        message(0, "iter=%d exchange of %013ld particles\n", iter, sumtogo);

        domain_exchange_once(layoutfunc, toGo, toGoSph, toGoBh,toGet, toGetSph, toGetBh);
        iter++;
    }
    while(ret > 0);

    myfree(toGetBh);
    myfree(toGetSph);
    myfree(toGet);
    myfree(toGoBh);
    myfree(toGoSph);
    myfree(toGo);
    /* Watch out: domain exchange changes the local number of particles.
     * though the slots has been taken care of in exchange_once, the
     * particle number counts are not updated. */
    domain_count_particles();
}

int domain_check_memory_bound(const int print_details, float *domainWork, int *domainCount)
{
    int ta, m, i;
    int load, max_load;
    int64_t sumload;
    double work, max_work, sumwork;
    /*Only used if print_details is true*/
    int list_load[NTask];
    double list_work[NTask];

    max_work = max_load = sumload = sumwork = 0;

    for(ta = 0; ta < NTask; ta++)
    {
        load = 0;
        work = 0;

        for(m = 0; m < All.DomainOverDecompositionFactor; m++)
            for(i = DomainStartList[ta * All.DomainOverDecompositionFactor + m]; i <= DomainEndList[ta * All.DomainOverDecompositionFactor + m]; i++)
            {
                load += domainCount[i];
                work += domainWork[i];
            }

        if(print_details) {
            list_load[ta] = load;
            list_work[ta] = work;
        }

        sumwork += work;
        sumload += load;

        if(load > max_load)
            max_load = load;
        if(work > max_work)
            max_work = work;
    }

    message(0, "Largest deviations from average: work=%g particle load=%g\n",
            max_work / (sumwork / NTask), max_load / (((double) sumload) / NTask));

    if(print_details) {
        message(0, "Balance breakdown:\n");
        for(i = 0; i < NTask; i++)
        {
            message(0, "Task: [%3d]  work=%8.4f  particle load=%8.4f\n", i,
               list_work[i] / (sumwork / NTask), list_load[i] / (((double) sumload) / NTask));
        }
    }

    if(max_load > All.MaxPart)
    {
        message(0, "desired memory imbalance=%g  (limit=%d, needed=%d)\n",
                    (max_load * All.PartAllocFactor) / All.MaxPart, All.MaxPart, max_load);

        return 1;
    }

    return 0;
}

static void domain_exchange_once(int (*layoutfunc)(int p), int* toGo, int * toGoSph, int * toGoBh, int *toGet, int *toGetSph, int *toGetBh)
{
    int count_togo = 0, count_togo_sph = 0, count_togo_bh = 0, 
        count_get = 0, count_get_sph = 0, count_get_bh = 0;

    int i, n, target;
    struct particle_data *partBuf;
    struct sph_particle_data *sphBuf;
    struct bh_particle_data *bhBuf;

    int * count = (int *) alloca(NTask * sizeof(int));
    int * count_sph = (int *) alloca(NTask * sizeof(int));
    int * count_bh = (int *) alloca(NTask * sizeof(int));
    int * offset = (int *) alloca(NTask * sizeof(int));
    int * offset_sph = (int *) alloca(NTask * sizeof(int));
    int * offset_bh = (int *) alloca(NTask * sizeof(int));

    int * count_recv = (int *) alloca(NTask * sizeof(int));
    int * count_recv_sph = (int *) alloca(NTask * sizeof(int));
    int * count_recv_bh = (int *) alloca(NTask * sizeof(int));
    int * offset_recv = (int *) alloca(NTask * sizeof(int));
    int * offset_recv_sph = (int *) alloca(NTask * sizeof(int));
    int * offset_recv_bh = (int *) alloca(NTask * sizeof(int));

    for(i = 1, offset_sph[0] = 0; i < NTask; i++)
        offset_sph[i] = offset_sph[i - 1] + toGoSph[i - 1];

    for(i = 1, offset_bh[0] = 0; i < NTask; i++)
        offset_bh[i] = offset_bh[i - 1] + toGoBh[i - 1];

    offset[0] = offset_sph[NTask - 1] + toGoSph[NTask - 1];

    for(i = 1; i < NTask; i++)
        offset[i] = offset[i - 1] + (toGo[i - 1] - toGoSph[i - 1]);

    for(i = 0; i < NTask; i++)
    {
        count_togo += toGo[i];
        count_togo_sph += toGoSph[i];
        count_togo_bh += toGoBh[i];

        count_get += toGet[i];
        count_get_sph += toGetSph[i];
        count_get_bh += toGetBh[i];
    }

    partBuf = (struct particle_data *) mymalloc("partBuf", count_togo * sizeof(struct particle_data));
    sphBuf = (struct sph_particle_data *) mymalloc("sphBuf", count_togo_sph * sizeof(struct sph_particle_data));
    bhBuf = (struct bh_particle_data *) mymalloc("bhBuf", count_togo_bh * sizeof(struct bh_particle_data));

    for(i = 0; i < NTask; i++)
        count[i] = count_sph[i] = count_bh[i] = 0;

    /*FIXME: make this omp ! */
    for(n = 0; n < NumPart; n++)
    {
        if(!(P[n].OnAnotherDomain && P[n].WillExport)) continue;
        /* preparing for export */
        P[n].OnAnotherDomain = 0;
        P[n].WillExport = 0;
        target = layoutfunc(n);

        if(P[n].Type == 0)
        {
            partBuf[offset_sph[target] + count_sph[target]] = P[n];
            sphBuf[offset_sph[target] + count_sph[target]] = SPHP(n);
            count_sph[target]++;
        } else
        if(P[n].Type == 5)
        {
            bhBuf[offset_bh[target] + count_bh[target]] = BhP[P[n].PI];
            /* points to the subbuffer */
            P[n].PI = count_bh[target];
            partBuf[offset[target] + count[target]] = P[n];
            count_bh[target]++;
            count[target]++;
        }
        else
        {
            partBuf[offset[target] + count[target]] = P[n];
            count[target]++;
        }


        if(P[n].Type == 0)
        {
            P[n] = P[N_sph_slots - 1];
            P[N_sph_slots - 1] = P[NumPart - 1];
            /* Because SphP doesn't use PI */
            SPHP(n) = SPHP(N_sph_slots - 1);

            NumPart--;
            N_sph_slots--;
            n--;
        }
        else
        {
            P[n] = P[NumPart - 1];
            NumPart--;
            n--;
        }
    }
    walltime_measure("/Domain/exchange/makebuf");

    for(i = 0; i < NTask; i ++) {
        if(count_sph[i] != toGoSph[i] ) {
            abort();
        }
        if(count_bh[i] != toGoBh[i] ) {
            abort();
        }
    }

    if(count_get_sph)
    {
        memmove(P + N_sph_slots + count_get_sph, P + N_sph_slots, (NumPart - N_sph_slots) * sizeof(struct particle_data));
    }

    for(i = 0; i < NTask; i++)
    {
        count_recv_sph[i] = toGetSph[i];
        count_recv_bh[i] = toGetBh[i];
        count_recv[i] = toGet[i] - toGetSph[i];
    }

    for(i = 1, offset_recv_sph[0] = N_sph_slots; i < NTask; i++)
        offset_recv_sph[i] = offset_recv_sph[i - 1] + count_recv_sph[i - 1];

    for(i = 1, offset_recv_bh[0] = N_bh_slots; i < NTask; i++)
        offset_recv_bh[i] = offset_recv_bh[i - 1] + count_recv_bh[i - 1];

    offset_recv[0] = NumPart + count_get_sph;

    for(i = 1; i < NTask; i++)
        offset_recv[i] = offset_recv[i - 1] + count_recv[i - 1];

    MPI_Alltoallv_sparse(partBuf, count_sph, offset_sph, MPI_TYPE_PARTICLE,
                 P, count_recv_sph, offset_recv_sph, MPI_TYPE_PARTICLE,
                 MPI_COMM_WORLD);
    walltime_measure("/Domain/exchange/alltoall");

    MPI_Alltoallv_sparse(sphBuf, count_sph, offset_sph, MPI_TYPE_SPHPARTICLE,
                 SphP, count_recv_sph, offset_recv_sph, MPI_TYPE_SPHPARTICLE,
                 MPI_COMM_WORLD);
    walltime_measure("/Domain/exchange/alltoall");

    MPI_Alltoallv_sparse(partBuf, count, offset, MPI_TYPE_PARTICLE,
                 P, count_recv, offset_recv, MPI_TYPE_PARTICLE,
                 MPI_COMM_WORLD);
    walltime_measure("/Domain/exchange/alltoall");

    MPI_Alltoallv_sparse(bhBuf, count_bh, offset_bh, MPI_TYPE_BHPARTICLE,
                BhP, count_recv_bh, offset_recv_bh, MPI_TYPE_BHPARTICLE,
                MPI_COMM_WORLD);
    walltime_measure("/Domain/exchange/alltoall");

    if(count_get_bh > 0) {
        for(target = 0; target < NTask; target++) {
            int i, j;
            for(i = offset_recv[target], j = offset_recv_bh[target];
                i < offset_recv[target] + count_recv[target]; i++) {
                if(P[i].Type != 5) continue;
                P[i].PI = j;
                j++;
            }
            if(j != count_recv_bh[target] + offset_recv_bh[target]) {
                endrun(1, "communication bh inconsistency\n");
            }
        }
    }

    NumPart += count_get;
    N_sph_slots += count_get_sph;
    N_bh_slots += count_get_bh;

    if(NumPart > All.MaxPart)
    {
        endrun(787878, "Task=%d NumPart=%d All.MaxPart=%d\n", ThisTask, NumPart, All.MaxPart);
    }

    if(N_sph_slots > All.MaxPart)
        endrun(787878, "Task=%d N_sph=%d All.MaxPart=%d\n", ThisTask, N_sph_slots, All.MaxPart);
    if(N_bh_slots > All.MaxPartBh)
        endrun(787878, "Task=%d N_bh=%d All.MaxPartBh=%d\n", ThisTask, N_bh_slots, All.MaxPartBh);

    myfree(bhBuf);
    myfree(sphBuf);
    myfree(partBuf);

    MPI_Barrier(MPI_COMM_WORLD);

    walltime_measure("/Domain/exchange/finalize");
}
static int bh_cmp_reverse_link(const void * b1in, const void * b2in) {
    const struct bh_particle_data * b1 = (struct bh_particle_data *) b1in;
    const struct bh_particle_data * b2 = (struct bh_particle_data *) b2in;
    if(b1->ReverseLink == -1 && b2->ReverseLink == -1) {
        return 0;
    }
    if(b1->ReverseLink == -1) return 1;
    if(b2->ReverseLink == -1) return -1;
    return (b1->ReverseLink > b2->ReverseLink) - (b1->ReverseLink < b2->ReverseLink);

}

static int
domain_bh_garbage_collection()
{
    /*
     *  BhP is a lifted structure. 
     *  changing BH slots doesn't affect tree's consistency;
     *  this function always return 0. */

    /* gc the bh */
    int i, j;
    int64_t total = 0;

    int64_t total0 = 0;

    sumup_large_ints(1, &N_bh_slots, &total0);

    /* If there are no blackholes, there cannot be any garbage. bail. */
    if(total0 == 0) return 0;

#pragma omp parallel for
    for(i = 0; i < All.MaxPartBh; i++) {
        BhP[i].ReverseLink = -1;
    }

#pragma omp parallel for
    for(i = 0; i < NumPart; i++) {
        if(P[i].Type == 5) {
            BhP[P[i].PI].ReverseLink = i;
            if(P[i].PI >= N_bh_slots) {
                endrun(1, "bh PI consistency failed2, N_bh_slots = %d, N_bh = %d, PI=%d\n", N_bh_slots, NLocal[5], P[i].PI);
            }
            if(BhP[P[i].PI].ID != P[i].ID) {
                endrun(1, "bh id consistency failed1\n");
            }
        }
    }

    /* put unused guys to the end, and sort the used ones
     * by their location in the P array */
    qsort(BhP, N_bh_slots, sizeof(BhP[0]), bh_cmp_reverse_link);

    while(N_bh_slots > 0 && BhP[N_bh_slots - 1].ReverseLink == -1) {
        N_bh_slots --;
    }
    /* Now update the link in BhP */
    for(i = 0; i < N_bh_slots; i ++) {
        P[BhP[i].ReverseLink].PI = i;
    }

    /* Now invalidate ReverseLink */
    for(i = 0; i < N_bh_slots; i ++) {
        BhP[i].ReverseLink = -1;
    }

    j = 0;
#pragma omp parallel for
    for(i = 0; i < NumPart; i++) {
        if(P[i].Type != 5) continue;
        if(P[i].PI >= N_bh_slots) {
            endrun(1, "bh PI consistency failed2\n");
        }
        if(BhP[P[i].PI].ID != P[i].ID) {
            endrun(1, "bh id consistency failed2\n");
        }
#pragma omp atomic
        j ++;
    }
    if(j != N_bh_slots) {
        endrun(1, "bh count failed2, j=%d, N_bh=%d\n", j, N_bh_slots);
    }

    sumup_large_ints(1, &N_bh_slots, &total);

    if(total != total0) {
        message(0, "GC: Reducing number of BH slots from %ld to %ld\n", total0, total);
    }
    /* bh gc never invalidates the tree */
    return 0;
}

int domain_fork_particle(int parent) {
    /* this will fork a zero mass particle at the given location of parent.
     *
     * Assumes the particle is protected by locks in threaded env.
     *
     * The Generation of parent is incremented.
     * The child carries the incremented generation number.
     * The ID of the child is modified, with the new generation number set
     * at the highest 8 bits.
     *
     * the new particle's index is returned.
     *
     * Its mass and ptype can be then adjusted. (watchout detached BH /SPH
     * data!)
     * Its PIndex still points to the old Pindex!
     * */

    if(NumPart >= All.MaxPart)
    {
        endrun(8888,
                "On Task=%d with NumPart=%d we try to spawn. Sorry, no space left...(All.MaxPart=%d)\n",
                ThisTask, NumPart, All.MaxPart);
    }
    /*This is all racy if ActiveParticle or P is accessed from another thread*/
    int child = atomic_fetch_and_add(&NumPart, 1);
    /*Update the active particle list*/
    int childactive = atomic_fetch_and_add(&NumActiveParticle, 1);
    ActiveParticle[childactive] = child;

    P[parent].Generation ++;
    uint64_t g = P[parent].Generation;
    /* change the child ID according to the generation. */
    P[child] = P[parent];
    P[child].ID = (P[parent].ID & 0x00ffffffffffffffL) + (g << 56L);

    /* the PIndex still points to the old PIndex */
    P[child].Mass = 0;

    /* FIXME: these are not thread safe !!not !!*/
    timebin_add_particle_to_active(parent, child, P[child].TimeBin);

    /* increase NumForceUpdate only if this particle was
     * active */

    /*! When a new additional star particle is created, we can put it into the
     *  tree at the position of the spawning gas particle. This is possible
     *  because the Nextnode[] array essentially describes the full tree walk as a
     *  link list. Multipole moments of tree nodes need not be changed.
     */

    /* we do this only if there is an active force tree 
     * checking Nextnode is not the best way of doing so though.
     * */
    if(Nextnode) {
        int no;

        no = Nextnode[parent];
        Nextnode[parent] = child;
        Nextnode[child] = no;
        Father[child] = Father[parent];
    }
    return child;
}



static struct domain_loadorigin_data
{
    double load;
    int origin;
}
*domain;

static struct domain_segments_data
{
    int task, start, end;
}
*domainAssign;



int domain_sort_loadorigin(const void *a, const void *b)
{
    if(((struct domain_loadorigin_data *) a)->load < (((struct domain_loadorigin_data *) b)->load))
        return -1;

    if(((struct domain_loadorigin_data *) a)->load > (((struct domain_loadorigin_data *) b)->load))
        return +1;

    return 0;
}

int domain_sort_segments(const void *a, const void *b)
{
    if(((struct domain_segments_data *) a)->task < (((struct domain_segments_data *) b)->task))
        return -1;

    if(((struct domain_segments_data *) a)->task > (((struct domain_segments_data *) b)->task))
        return +1;

    return 0;
}


void domain_assign_balanced(float *domainWork, int *domainCount)
{
    int i, n, ndomains, *target;

    domainAssign =
        (struct domain_segments_data *) mymalloc("domainAssign",
                All.DomainOverDecompositionFactor * NTask * sizeof(struct domain_segments_data));
    domain =
        (struct domain_loadorigin_data *) mymalloc("domain",
                All.DomainOverDecompositionFactor * NTask *
                sizeof(struct domain_loadorigin_data));
    target = (int *) mymalloc("target", All.DomainOverDecompositionFactor * NTask * sizeof(int));

    for(n = 0; n < All.DomainOverDecompositionFactor * NTask; n++)
        domainAssign[n].task = n;

    ndomains = All.DomainOverDecompositionFactor * NTask;

    while(ndomains > NTask)
    {
        for(i = 0; i < ndomains; i++)
        {
            domain[i].load = 0;
            domain[i].origin = i;
        }

        for(n = 0; n < All.DomainOverDecompositionFactor * NTask; n++)
        {
            for(i = DomainStartList[n]; i <= DomainEndList[n]; i++)
                if(domainWork)
                    domain[domainAssign[n].task].load += domainWork[i];
                else if(domainCount)
                    domain[domainAssign[n].task].load += domainCount[i];
        }

        qsort(domain, ndomains, sizeof(struct domain_loadorigin_data), domain_sort_loadorigin);

        for(i = 0; i < ndomains / 2; i++)
        {
            target[domain[i].origin] = i;
            target[domain[ndomains - 1 - i].origin] = i;
        }

        for(n = 0; n < All.DomainOverDecompositionFactor * NTask; n++)
            domainAssign[n].task = target[domainAssign[n].task];

        ndomains /= 2;
    }

    for(n = 0; n < All.DomainOverDecompositionFactor * NTask; n++)
    {
        domainAssign[n].start = DomainStartList[n];
        domainAssign[n].end = DomainEndList[n];
    }

    qsort(domainAssign, All.DomainOverDecompositionFactor * NTask, sizeof(struct domain_segments_data), domain_sort_segments);

    for(n = 0; n < All.DomainOverDecompositionFactor * NTask; n++)
    {
        DomainStartList[n] = domainAssign[n].start;
        DomainEndList[n] = domainAssign[n].end;

        for(i = DomainStartList[n]; i <= DomainEndList[n]; i++)
            DomainTask[i] = domainAssign[n].task;
    }

    myfree(target);
    myfree(domain);
    myfree(domainAssign);
}


/* These next two functions are identical except for the type of the thing summed.*/
void domain_findSplit_work_balanced(int ncpu, int ndomain, float *domainWork)
{
    int i, start, end;
    double work, workavg, work_before, workavg_before;

    for(i = 0, work = 0; i < ndomain; i++)
        work += domainWork[i];

    workavg = work / ncpu;

    work_before = workavg_before = 0;

    start = 0;

    for(i = 0; i < ncpu; i++)
    {
        work = 0;
        end = start;

        work += domainWork[end];

        while((work + work_before < workavg + workavg_before) || (i == ncpu - 1 && end < ndomain - 1))
        {
            if((ndomain - end) > (ncpu - i))
                end++;
            else
                break;

            work += domainWork[end];
        }

        DomainStartList[i] = start;
        DomainEndList[i] = end;

        work_before += work;
        workavg_before += workavg;
        start = end + 1;
    }
}

void domain_findSplit_load_balanced(int ncpu, int ndomain, int *domainCount)
{
    int i, start, end;
    double load, loadavg, load_before, loadavg_before;

    for(i = 0, load = 0; i < ndomain; i++)
        load += domainCount[i];

    loadavg = load / ncpu;

    load_before = loadavg_before = 0;

    start = 0;

    for(i = 0; i < ncpu; i++)
    {
        load = 0;
        end = start;

        load += domainCount[end];

        while((load + load_before < loadavg + loadavg_before) || (i == ncpu - 1 && end < ndomain - 1))
        {
            if((ndomain - end) > (ncpu - i))
                end++;
            else
                break;

            load += domainCount[end];
        }

        DomainStartList[i] = start;
        DomainEndList[i] = end;

        load_before += load;
        loadavg_before += loadavg;
        start = end + 1;
    }
}


/*This function determines the leaf node for the given particle number.*/
static inline int domain_leafnodefunc(const peanokey key) {
    int no=0;
    while(topNodes[no].Daughter >= 0)
        no = topNodes[no].Daughter + (key - topNodes[no].StartKey) / (topNodes[no].Size / 8);
    no = topNodes[no].Leaf;
    return no;
}

/*! This function determines how many particles that are currently stored
 *  on the local CPU have to be moved off according to the domain
 *  decomposition.
 *
 *  layoutfunc decides the target Task of particle p (used by
 *  subfind_distribute).
 *
 */
static int domain_layoutfunc(int n) {
    peanokey key = P[n].Key;
    int no = domain_leafnodefunc(key);
    return DomainTask[no];
}

/*This function populates the toGo and toGet arrays*/
static int domain_countToGo(ptrdiff_t nlimit, int (*layoutfunc)(int p), int* toGo, int * toGoSph, int * toGoBh, int *toGet, int *toGetSph, int *toGetBh)
{
    int n, ret, retsum;
    size_t package;

    int * list_NumPart = (int *) alloca(sizeof(int) * NTask);
    int * list_N_sph = (int *) alloca(sizeof(int) * NTask);
    int * list_N_bh = (int *) alloca(sizeof(int) * NTask);

    for(n = 0; n < NTask; n++)
    {
        toGo[n] = 0;
        toGoSph[n] = 0;
        toGoBh[n] = 0;
    }

    package = (sizeof(struct particle_data) + sizeof(struct sph_particle_data) + sizeof(struct bh_particle_data));
    if(package >= nlimit)
        endrun(212, "Package is too large, no free memory.");


    for(n = 0; n < NumPart; n++)
    {
        if(package >= nlimit) break;
        if(!P[n].OnAnotherDomain) continue;

        int target = layoutfunc(n);
        if (target == ThisTask) continue;

        toGo[target] += 1;
        nlimit -= sizeof(struct particle_data);

        if(P[n].Type  == 0)
        {
            toGoSph[target] += 1;
            nlimit -= sizeof(struct sph_particle_data);
        }
        if(P[n].Type  == 5)
        {
            toGoBh[target] += 1;
            nlimit -= sizeof(struct bh_particle_data);
        }
        P[n].WillExport = 1;	/* flag this particle for export */
    }

    MPI_Alltoall(toGo, 1, MPI_INT, toGet, 1, MPI_INT, MPI_COMM_WORLD);
    MPI_Alltoall(toGoSph, 1, MPI_INT, toGetSph, 1, MPI_INT, MPI_COMM_WORLD);
    MPI_Alltoall(toGoBh, 1, MPI_INT, toGetBh, 1, MPI_INT, MPI_COMM_WORLD);

    if(package >= nlimit)
        ret = 1;
    else
        ret = 0;

    MPI_Allreduce(&ret, &retsum, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

    if(retsum)
    {
        /* in this case, we are not guaranteed that the temporary state after
           the partial exchange will actually observe the particle limits on all
           processors... we need to test this explicitly and rework the exchange
           such that this is guaranteed. This is actually a rather non-trivial
           constraint. */

        MPI_Allgather(&NumPart, 1, MPI_INT, list_NumPart, 1, MPI_INT, MPI_COMM_WORLD);
        MPI_Allgather(&N_bh_slots, 1, MPI_INT, list_N_bh, 1, MPI_INT, MPI_COMM_WORLD);
        MPI_Allgather(&N_sph_slots, 1, MPI_INT, list_N_sph, 1, MPI_INT, MPI_COMM_WORLD);

        int flag, flagsum, ntoomany, ta, i;
        int count_togo, count_toget, count_togo_bh, count_toget_bh, count_togo_sph, count_toget_sph;

        do
        {
            flagsum = 0;

            do
            {
                flag = 0;

                for(ta = 0; ta < NTask; ta++)
                {
                    if(ta == ThisTask)
                    {
                        count_togo = count_toget = 0;
                        count_togo_sph = count_toget_sph = 0;
                        count_togo_bh = count_toget_bh = 0;
                        for(i = 0; i < NTask; i++)
                        {
                            count_togo += toGo[i];
                            count_toget += toGet[i];
                            count_togo_sph += toGoSph[i];
                            count_toget_sph += toGetSph[i];
                            count_togo_bh += toGoBh[i];
                            count_toget_bh += toGetBh[i];
                        }
                    }
                    MPI_Bcast(&count_togo, 1, MPI_INT, ta, MPI_COMM_WORLD);
                    MPI_Bcast(&count_toget, 1, MPI_INT, ta, MPI_COMM_WORLD);
                    MPI_Bcast(&count_togo_sph, 1, MPI_INT, ta, MPI_COMM_WORLD);
                    MPI_Bcast(&count_toget_sph, 1, MPI_INT, ta, MPI_COMM_WORLD);
                    MPI_Bcast(&count_togo_bh, 1, MPI_INT, ta, MPI_COMM_WORLD);
                    MPI_Bcast(&count_toget_bh, 1, MPI_INT, ta, MPI_COMM_WORLD);
                    if((ntoomany = list_N_sph[ta] + count_toget_sph - count_togo_sph - All.MaxPart) > 0)
                    {
                        message (0, "exchange needs to be modified because I can't receive %d SPH-particles on task=%d\n",
                                 ntoomany, ta);
                        if(flagsum > 25) {
                            message(0, "list_N_sph[ta=%d]=%d  count_toget_sph=%d count_togo_sph=%d\n",
                                        ta, list_N_sph[ta], count_toget_sph, count_togo_sph);
                        }
                        flag = 1;
                        i = flagsum % NTask;
                        while(ntoomany)
                        {
                            if(i == ThisTask)
                            {
                                if(toGoSph[ta] > 0)
                                {
                                    toGoSph[ta]--;
                                    count_toget_sph--;
                                    count_toget--;
                                    ntoomany--;
                                }
                            }

                            MPI_Bcast(&ntoomany, 1, MPI_INT, i, MPI_COMM_WORLD);
                            MPI_Bcast(&count_toget, 1, MPI_INT, i, MPI_COMM_WORLD);
                            MPI_Bcast(&count_toget_sph, 1, MPI_INT, i, MPI_COMM_WORLD);
                            i++;
                            if(i >= NTask)
                                i = 0;
                        }
                    }
                    if((ntoomany = list_N_bh[ta] + count_toget_bh - count_togo_bh - All.MaxPartBh) > 0)
                    {
                        message(0, "exchange needs to be modified because I can't receive %d BH-particles on task=%d\n",
                                ntoomany, ta);
                        if(flagsum > 25)
                            message(0, "list_N_bh[ta=%d]=%d  count_toget_bh=%d count_togo_bh=%d\n",
                                    ta, list_N_bh[ta], count_toget_bh, count_togo_bh);

                        flag = 1;
                        i = flagsum % NTask;
                        while(ntoomany)
                        {
                            if(i == ThisTask)
                            {
                                if(toGoBh[ta] > 0)
                                {
                                    toGoBh[ta]--;
                                    count_toget_bh--;
                                    count_toget--;
                                    ntoomany--;
                                }
                            }

                            MPI_Bcast(&ntoomany, 1, MPI_INT, i, MPI_COMM_WORLD);
                            MPI_Bcast(&count_toget, 1, MPI_INT, i, MPI_COMM_WORLD);
                            MPI_Bcast(&count_toget_bh, 1, MPI_INT, i, MPI_COMM_WORLD);
                            i++;
                            if(i >= NTask)
                                i = 0;
                        }
                    }

                    if((ntoomany = list_NumPart[ta] + count_toget - count_togo - All.MaxPart) > 0)
                    {
                        message (0, "exchange needs to be modified because I can't receive %d particles on task=%d\n",
                             ntoomany, ta);
                        if(flagsum > 25)
                            message(0, "list_NumPart[ta=%d]=%d  count_toget=%d count_togo=%d\n",
                                    ta, list_NumPart[ta], count_toget, count_togo);

                        flag = 1;
                        i = flagsum % NTask;
                        while(ntoomany)
                        {
                            if(i == ThisTask)
                            {
                                if(toGo[ta] > 0)
                                {
                                    toGo[ta]--;
                                    count_toget--;
                                    ntoomany--;
                                }
                            }

                            MPI_Bcast(&ntoomany, 1, MPI_INT, i, MPI_COMM_WORLD);
                            MPI_Bcast(&count_toget, 1, MPI_INT, i, MPI_COMM_WORLD);

                            i++;
                            if(i >= NTask)
                                i = 0;
                        }
                    }
                }
                flagsum += flag;

                message(0, "flagsum = %d\n", flagsum);
                if(flagsum > 100)
                    endrun(1013, "flagsum is too big, what does this mean?");
            }
            while(flag);

            if(flagsum)
            {
                int *local_toGo, *local_toGoSph, *local_toGoBh;

                local_toGo = (int *)mymalloc("	      local_toGo", NTask * sizeof(int));
                local_toGoSph = (int *)mymalloc("	      local_toGoSph", NTask * sizeof(int));
                local_toGoBh = (int *)mymalloc("	      local_toGoBh", NTask * sizeof(int));


                for(n = 0; n < NTask; n++)
                {
                    local_toGo[n] = 0;
                    local_toGoSph[n] = 0;
                    local_toGoBh[n] = 0;
                }

                for(n = 0; n < NumPart; n++)
                {
                    if(!P[n].OnAnotherDomain) continue;
                    P[n].WillExport = 0; /* clear 16 */

                    int target = layoutfunc(n);

                    if(P[n].Type == 0)
                    {
                        if(local_toGoSph[target] < toGoSph[target] && local_toGo[target] < toGo[target])
                        {
                            local_toGo[target] += 1;
                            local_toGoSph[target] += 1;
                            P[n].WillExport = 1;
                        }
                    }
                    else
                    if(P[n].Type == 5)
                    {
                        if(local_toGoBh[target] < toGoBh[target] && local_toGo[target] < toGo[target])
                        {
                            local_toGo[target] += 1;
                            local_toGoBh[target] += 1;
                            P[n].WillExport = 1;
                        }
                    }
                    else
                    {
                        if(local_toGo[target] < toGo[target])
                        {
                            local_toGo[target] += 1;
                            P[n].WillExport = 1;
                        }
                    }
                }

                for(n = 0; n < NTask; n++)
                {
                    toGo[n] = local_toGo[n];
                    toGoSph[n] = local_toGoSph[n];
                    toGoBh[n] = local_toGoBh[n];
                }

                MPI_Alltoall(toGo, 1, MPI_INT, toGet, 1, MPI_INT, MPI_COMM_WORLD);
                MPI_Alltoall(toGoSph, 1, MPI_INT, toGetSph, 1, MPI_INT, MPI_COMM_WORLD);
                MPI_Alltoall(toGoBh, 1, MPI_INT, toGetBh, 1, MPI_INT, MPI_COMM_WORLD);
                myfree(local_toGoBh);
                myfree(local_toGoSph);
                myfree(local_toGo);
            }
        }
        while(flagsum);

        return 1;
    }
    else
        return 0;
}






/*! This function walks the global top tree in order to establish the
 *  number of leaves it has. These leaves are distributed to different
 *  processors.
 */
void domain_walktoptree(int no)
{
    int i;

    if(topNodes[no].Daughter == -1)
    {
        topNodes[no].Leaf = NTopleaves;
        NTopleaves++;
    }
    else
    {
        for(i = 0; i < 8; i++)
            domain_walktoptree(topNodes[no].Daughter + i);
    }
}

/* Refine the local oct-tree, recursively adding costs and particles until
 * either we have chopped off all the peano-hilbert keys and thus have no more
 * refinement to do, or we run out of topNodes.
 * If 1 is returned on any processor we will return to domain_Decomposition,
 * allocate 30% more topNodes, and try again.
 * */
int domain_check_for_local_refine(const int i, const struct peano_hilbert_data * mp)
{
    int j, p;

    /*If there are only 8 particles within this node, we are done refining.*/
    if(topNodes[i].Size < 8)
        return 0;

    /* We need to do refinement if (if we have a parent) we have more than 80%
     * of the parent's particles or costs.*/
    /* If we were below them but we have a parent and somehow got all of its particles, we still
     * need to refine. But if none of these things are true we can return, our work complete. */
    if(topNodes[i].Parent < 0 || (topNodes[i].Count <= 0.8 * topNodes[topNodes[i].Parent].Count &&
            topNodes[i].Cost <= 0.8 * topNodes[topNodes[i].Parent].Cost))
        return 0;

    /* If we want to refine but there is no space for another topNode on this processor,
     * we ran out of top nodes and must get more.*/
    if((NTopnodes + 8) > MaxTopNodes)
        return 1;

    /*Make a new topnode section attached to this node*/
    topNodes[i].Daughter = NTopnodes;
    NTopnodes += 8;

    /* Initialise this topnode with new sub nodes*/
    for(j = 0; j < 8; j++)
    {
        const int sub = topNodes[i].Daughter + j;
        /* The new sub nodes have this node as parent
         * and no daughters.*/
        topNodes[sub].Daughter = -1;
        topNodes[sub].Parent = i;
        /* Shorten the peano key by a factor 8, reflecting the oct-tree level.*/
        topNodes[sub].Size = (topNodes[i].Size >> 3);
        /* This is the region of peanospace covered by this node.*/
        topNodes[sub].StartKey = topNodes[i].StartKey + j * topNodes[sub].Size;
        /* We will compute the cost and initialise the first particle in the node below.
         * This PIndex value is never used*/
        topNodes[sub].PIndex = topNodes[i].PIndex;
        topNodes[sub].Count = 0;
        topNodes[sub].Cost = 0;
    }

    /* Loop over all particles in this node so that the costs of the daughter nodes are correct*/
    for(p = 0, j = 0; p < topNodes[i].Count; p++)
    {
        const int sub = topNodes[i].Daughter;

        /* This identifies which subnode this particle belongs to.
         * Once this particle has passed the StartKey of the next daughter node,
         * we increment the node the particle is added to and set the PIndex.*/
        if(j < 7)
            while(topNodes[sub + j + 1].StartKey <= mp[p + topNodes[i].PIndex].key)
            {
                topNodes[sub + j + 1].PIndex = p;
                j++;
                if(j >= 7)
                    break;
            }

        /*Now we have identified the subnode for this particle, add it to the cost and count*/
        topNodes[sub+j].Cost += domain_particle_costfactor(mp[p + topNodes[i].PIndex].index);
        topNodes[sub+j].Count++;
    }

    /*Check and refine the new daughter nodes*/
    for(j = 0; j < 8; j++)
    {
        const int sub = topNodes[i].Daughter + j;
        /* Refine each sub node. If we could not refine the node as needed,
         * we are out of node space and need more.*/
        if(domain_check_for_local_refine(sub, mp))
            return 1;
    }
    return 0;
}

int domain_nonrecursively_combine_topTree()
{
    /* 
     * combine topTree non recursively, this uses MPI_Bcast within a group.
     * it shall be quite a bit faster (~ x2) than the old recursive scheme.
     *
     * it takes less time at higher sep.
     *
     * The communicate should have been done with MPI Inter communicator.
     * but I couldn't figure out how to do it that way.
     * */
    int sep = 1;
    MPI_Datatype MPI_TYPE_TOPNODE;
    MPI_Type_contiguous(sizeof(struct local_topnode_data), MPI_BYTE, &MPI_TYPE_TOPNODE);
    MPI_Type_commit(&MPI_TYPE_TOPNODE);
    int errorflag = 0;
    int errorflagall = 0;

    for(sep = 1; sep < NTask; sep *=2) {
        /* build the subcommunicators for broadcasting */
        int Color = ThisTask / sep;
        int Key = ThisTask % sep;
        int ntopnodes_import = 0;
        struct local_topnode_data * topNodes_import = NULL;

        int recvTask = -1; /* by default do not communicate */

        if(Key != 0) {
            /* non leaders will skip exchanges */
            goto loop_continue;
        }

        /* leaders of even color will combine nodes from next odd color,
         * so that when sep is increased eventually rank 0 will have all
         * nodes */
        if(Color % 2 == 0) {
            /* even guys recv */
            recvTask = ThisTask + sep;
            if(recvTask < NTask) {
                MPI_Recv(
                        &ntopnodes_import, 1, MPI_INT, recvTask, TAG_GRAV_A,
                        MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                topNodes_import = (struct local_topnode_data *) mymalloc("topNodes_import",
                            IMAX(ntopnodes_import, NTopnodes) * sizeof(struct local_topnode_data));

                MPI_Recv(
                        topNodes_import,
                        ntopnodes_import, MPI_TYPE_TOPNODE, 
                        recvTask, TAG_GRAV_B, 
                        MPI_COMM_WORLD, MPI_STATUS_IGNORE);


                if((NTopnodes + ntopnodes_import) > MaxTopNodes) {
                    errorflag = 1;
                } else {
                    if(ntopnodes_import < 0) {
                        endrun(1, "severe domain error using a unintended rank \n");
                    }
                    if(ntopnodes_import > 0 ) {
                        domain_insertnode(topNodes, topNodes_import, 0, 0);
                    } 
                }
                myfree(topNodes_import);
            }
        } else {
            /* odd guys send */
            recvTask = ThisTask - sep;
            if(recvTask >= 0) {
                MPI_Send(&NTopnodes, 1, MPI_INT, recvTask, TAG_GRAV_A,
                        MPI_COMM_WORLD);
                MPI_Send(topNodes,
                        NTopnodes, MPI_TYPE_TOPNODE,
                        recvTask, TAG_GRAV_B,
                        MPI_COMM_WORLD);
            }
            NTopnodes = -1;
        }

loop_continue:
        MPI_Allreduce(&errorflag, &errorflagall, 1, MPI_INT, MPI_LOR, MPI_COMM_WORLD);
        if(errorflagall) {
            break;
        }
    }

    MPI_Bcast(&NTopnodes, 1, MPI_INT, 0, MPI_COMM_WORLD);
    MPI_Bcast(topNodes, NTopnodes, MPI_TYPE_TOPNODE, 0, MPI_COMM_WORLD);
    MPI_Type_free(&MPI_TYPE_TOPNODE);
    return errorflagall;
}

/*! This function constructs the global top-level tree node that is used
 *  for the domain decomposition. This is done by considering the string of
 *  Peano-Hilbert keys for all particles, which is recursively chopped off
 *  in pieces of eight segments until each segment holds at most a certain
 *  number of particles.
 */
int domain_determineTopTree(void)
{
    int i, j, sub;
    int errflag, errsum;
    double costlimit, countlimit;

    struct peano_hilbert_data * mp = (struct peano_hilbert_data *) mymalloc("mp", sizeof(struct peano_hilbert_data) * NumPart);

    #pragma omp parallel for
    for(i = 0; i < NumPart; i++)
    {
        mp[i].key = P[i].Key;
        mp[i].index = i;
    }

    walltime_measure("/Domain/DetermineTopTree/Misc");
    qsort_openmp(mp, NumPart, sizeof(struct peano_hilbert_data), peano_compare_key);
    
    walltime_measure("/Domain/DetermineTopTree/Sort");

    double totgravcost, gravcost = 0;
#pragma omp parallel for reduction(+: gravcost)
    for(i = 0; i < NumPart; i++)
    {
        float costfac = domain_particle_costfactor(i);
        gravcost += costfac;
    }

    MPI_Allreduce(&gravcost, &totgravcost, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

    NTopnodes = 1;
    topNodes[0].Daughter = -1;
    topNodes[0].Parent = -1;
    topNodes[0].Size = PEANOCELLS;
    topNodes[0].StartKey = 0;
    topNodes[0].PIndex = 0;
    topNodes[0].Count = NumPart;
    topNodes[0].Cost = gravcost;

    costlimit = totgravcost / (TOPNODEFACTOR * All.DomainOverDecompositionFactor * NTask);
    countlimit = TotNumPart / (TOPNODEFACTOR * All.DomainOverDecompositionFactor * NTask);

    errflag = domain_check_for_local_refine(0, mp);
    walltime_measure("/Domain/DetermineTopTree/LocalRefine");

    myfree(mp);

    MPI_Allreduce(&errflag, &errsum, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    if(errsum)
    {
        message(0, "We are out of Topnodes. We'll try to repeat with a higher value than All.TopNodeAllocFactor=%g\n",
                 All.TopNodeAllocFactor);
        return errsum;
    }


    /* we now need to exchange tree parts and combine them as needed */

    
    errflag = domain_nonrecursively_combine_topTree();

    walltime_measure("/Domain/DetermineTopTree/Combine");
#if 0
    char buf[1000];
    sprintf(buf, "topnodes.bin.%d", ThisTask);
    FILE * fd = fopen(buf, "w");

    /* these PIndex are non-essential in other modules, so we reset them */
    for(i = 0; i < NTopnodes; i ++) {
        topNodes[i].PIndex = -1;
    }
    fwrite(topNodes, sizeof(struct local_topnode_data), NTopnodes, fd);
    fclose(fd);

    //MPI_Barrier(MPI_COMM_WORLD);
    //MPI_Abort(MPI_COMM_WORLD, 0);
#endif
    MPI_Allreduce(&errflag, &errsum, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

    if(errsum)
    {
        message(0, "can't combine trees due to lack of storage. Will try again.\n");
        return errsum;
    }

    /* now let's see whether we should still append more nodes, based on the estimated cumulative cost/count in each cell */

    message(0, "Before=%d\n", NTopnodes);

    for(i = 0, errflag = 0; i < NTopnodes; i++)
    {
        if(topNodes[i].Daughter < 0)
            if(topNodes[i].Count > countlimit || topNodes[i].Cost > costlimit)	/* ok, let's add nodes if we can */
                if(topNodes[i].Size > 1)
                {
                    if((NTopnodes + 8) <= MaxTopNodes)
                    {
                        topNodes[i].Daughter = NTopnodes;

                        for(j = 0; j < 8; j++)
                        {
                            sub = topNodes[i].Daughter + j;
                            topNodes[sub].Size = (topNodes[i].Size >> 3);
                            topNodes[sub].Count = topNodes[i].Count / 8;
                            topNodes[sub].Cost = topNodes[i].Cost / 8;
                            topNodes[sub].Daughter = -1;
                            topNodes[sub].Parent = i;
                            topNodes[sub].StartKey = topNodes[i].StartKey + j * topNodes[sub].Size;
                        }

                        NTopnodes += 8;
                    }
                    else
                    {
                        errflag = 1;
                        break;
                    }
                }
    }

    MPI_Allreduce(&errflag, &errsum, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    if(errsum)
        return errsum;

    message(0, "After=%d\n", NTopnodes);
    walltime_measure("/Domain/DetermineTopTree/Addnodes");

    return 0;
}



void domain_sumCost(float *domainWork, int *domainCount)
{
    int i;
    float * local_domainWork = (float *) mymalloc("local_domainWork", All.NumThreads * NTopnodes * sizeof(float));
    int * local_domainCount = (int *) mymalloc("local_domainCount", All.NumThreads * NTopnodes * sizeof(int));

    memset(local_domainWork, 0, All.NumThreads * NTopnodes * sizeof(float));
    memset(local_domainCount, 0, All.NumThreads * NTopnodes * sizeof(float));

    NTopleaves = 0;
    domain_walktoptree(0);

    message(0, "NTopleaves= %d  NTopnodes=%d (space for %d)\n", NTopleaves, NTopnodes, MaxTopNodes);

#pragma omp parallel
    {
        int tid = omp_get_thread_num();
        int n;

        float * mylocal_domainWork = local_domainWork + tid * NTopleaves;
        int * mylocal_domainCount = local_domainCount + tid * NTopleaves;

        #pragma omp for
        for(n = 0; n < NumPart; n++)
        {
            int no = domain_leafnodefunc(P[n].Key);

            mylocal_domainWork[no] += domain_particle_costfactor(n);

            mylocal_domainCount[no] += 1;
        }
    }

#pragma omp parallel for
    for(i = 0; i < NTopleaves; i++)
    {
        int tid;
        for(tid = 1; tid < All.NumThreads; tid++) {
            local_domainWork[i] += local_domainWork[i + tid * NTopleaves];
            local_domainCount[i] += local_domainCount[i + tid * NTopleaves];
        }
    }

    MPI_Allreduce(local_domainWork, domainWork, NTopleaves, MPI_FLOAT, MPI_SUM, MPI_COMM_WORLD);
    MPI_Allreduce(local_domainCount, domainCount, NTopleaves, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    myfree(local_domainCount);
    myfree(local_domainWork);
}

void domain_add_cost(struct local_topnode_data *treeA, int noA, int64_t count, double cost)
{
    int i, sub;
    int64_t countA, countB;

    countB = count / 8;
    countA = count - 7 * countB;

    cost = cost / 8;

    for(i = 0; i < 8; i++)
    {
        sub = treeA[noA].Daughter + i;

        if(i == 0)
            count = countA;
        else
            count = countB;

        treeA[sub].Count += count;
        treeA[sub].Cost += cost;

        if(treeA[sub].Daughter >= 0)
            domain_add_cost(treeA, sub, count, cost);
    }
}


void domain_insertnode(struct local_topnode_data *treeA, struct local_topnode_data *treeB, int noA, int noB)
{
    int j, sub;
    int64_t count, countA, countB;
    double cost, costA, costB;

    if(treeB[noB].Size < treeA[noA].Size)
    {
        if(treeA[noA].Daughter < 0)
        {
            if((NTopnodes + 8) <= MaxTopNodes)
            {
                count = treeA[noA].Count - treeB[treeB[noB].Parent].Count;
                countB = count / 8;
                countA = count - 7 * countB;

                cost = treeA[noA].Cost - treeB[treeB[noB].Parent].Cost;
                costB = cost / 8;
                costA = cost - 7 * costB;

                treeA[noA].Daughter = NTopnodes;
                for(j = 0; j < 8; j++)
                {
                    if(j == 0)
                    {
                        count = countA;
                        cost = costA;
                    }
                    else
                    {
                        count = countB;
                        cost = costB;
                    }

                    sub = treeA[noA].Daughter + j;
                    topNodes[sub].Size = (treeA[noA].Size >> 3);
                    topNodes[sub].Count = count;
                    topNodes[sub].Cost = cost;
                    topNodes[sub].Daughter = -1;
                    topNodes[sub].Parent = noA;
                    topNodes[sub].StartKey = treeA[noA].StartKey + j * treeA[sub].Size;
                }
                NTopnodes += 8;
            }
            else
                endrun(88, "Too many Topnodes");
        }

        sub = treeA[noA].Daughter + (treeB[noB].StartKey - treeA[noA].StartKey) / (treeA[noA].Size >> 3);
        domain_insertnode(treeA, treeB, sub, noB);
    }
    else if(treeB[noB].Size == treeA[noA].Size)
    {
        treeA[noA].Count += treeB[noB].Count;
        treeA[noA].Cost += treeB[noB].Cost;

        if(treeB[noB].Daughter >= 0)
        {
            for(j = 0; j < 8; j++)
            {
                sub = treeB[noB].Daughter + j;
                domain_insertnode(treeA, treeB, noA, sub);
            }
        }
        else
        {
            if(treeA[noA].Daughter >= 0)
                domain_add_cost(treeA, noA, treeB[noB].Count, treeB[noB].Cost);
        }
    }
    else
        endrun(89, "The tree is corrupted, cannot merge them. What is the invariance here?");
}


/* remove mass = 0 particles, holes in sph chunk and holes in bh buffer;
 * returns 1 if tree / timebin is invalid */
int
domain_garbage_collection(void)
{
    int tree_invalid = 0;

    /* tree is invalidated of the sequence on P is reordered; */
    /* TODO: in principle we can track this change and modify the tree nodes;
     * But doing so requires cleaning up the TimeBin link lists, and the tree
     * link lists first. likely worth it, since GC happens only in domain decompose
     * and snapshot IO, both take far more time than rebuilding the tree. */
    tree_invalid |= domain_sph_garbage_collection_reclaim();
    tree_invalid |= domain_all_garbage_collection();
    tree_invalid |= domain_bh_garbage_collection();

    MPI_Allreduce(MPI_IN_PLACE, &tree_invalid, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

    domain_count_particles();

    return tree_invalid;
}

void
domain_refresh_totals()
{
    int ptype;
    /* because NTotal[] is of type `int64_t', we cannot do a simple
     * MPI_Allreduce() to sum the total particle numbers 
     */
    MPI_Allreduce(NLocal, NTotal, 6, MPI_LONG, MPI_SUM, MPI_COMM_WORLD);

    TotNumPart = 0;
    for(ptype = 0; ptype < 6; ptype ++) {
        TotNumPart += NTotal[ptype];
    }
}

static int
domain_sph_garbage_collection_reclaim()
{
    int tree_invalid = 0;

    int64_t total0, total;
    sumup_large_ints(1, &N_sph_slots, &total0);

#ifdef SFR
    int i;
    for(i = 0; i < N_sph_slots; i++) {
        while(P[i].Type != 0 && i < N_sph_slots) {
            /* remove this particle from SphP, because
             * it is no longer a SPH
             * */
            /* note that when i == N-sph - 1 this doesn't really do any
             * thing. no harm done */
            struct particle_data psave;
            psave = P[i];
            P[i] = P[N_sph_slots - 1];
            SPHP(i) = SPHP(N_sph_slots - 1);
            P[N_sph_slots - 1] = psave;
            tree_invalid = 1;
            N_sph_slots --;
        }
    }
#endif
    sumup_large_ints(1, &N_sph_slots, &total);
    if(total != total0) {
        message(0, "GC: Reclaiming SPH slots from %ld to %ld\n", total0, total);
    }
    return tree_invalid;
}

static int
domain_all_garbage_collection()
{
    int i, tree_invalid = 0; 
    int count_elim, count_gaselim;
    int64_t total0, total;
    int64_t total0_gas, total_gas;

    sumup_large_ints(1, &N_sph_slots, &total0_gas);
    sumup_large_ints(1, &NumPart, &total0);

    count_elim = 0;
    count_gaselim = 0;

    for(i = 0; i < NumPart; i++)
        if(P[i].Mass == 0)
        {
            TimeBinCount[P[i].TimeBin]--;

            if(P[i].Type == 0)
            {
                TimeBinCountSph[P[i].TimeBin]--;

                P[i] = P[N_sph_slots - 1];
                SPHP(i) = SPHP(N_sph_slots - 1);

                P[N_sph_slots - 1] = P[NumPart - 1];

                N_sph_slots--;

                count_gaselim++;
            } else
            {
                P[i] = P[NumPart - 1];
            }

            NumPart--;
            i--;

            count_elim++;
        }

    sumup_large_ints(1, &N_sph_slots, &total_gas);
    sumup_large_ints(1, &NumPart, &total);

    if(total_gas != total0_gas) {
        message(0, "GC : Reducing SPH slots from %ld to %ld\n", total0_gas, total_gas);
    }

    if(total != total0) {
        message(0, "GC : Reducing Particle slots from %ld to %ld\n", total0, total);
        tree_invalid = 1;
    }
    return tree_invalid;
}

static void radix_id(const void * data, void * radix, void * arg) {
    ((uint64_t *) radix)[0] = ((MyIDType*) data)[0];
}

void
domain_test_id_uniqueness(void)
{
    int i;
    double t0, t1;
    MyIDType *ids, *ids_first;

    message(0, "Testing ID uniqueness...\n");

    if(NumPart == 0)
    {
        endrun(8, "need at least one particle per cpu\n");
    }

    t0 = second();

    ids = (MyIDType *) mymalloc("ids", NumPart * sizeof(MyIDType));
    ids_first = (MyIDType *) mymalloc("ids_first", NTask * sizeof(MyIDType));

    for(i = 0; i < NumPart; i++)
        ids[i] = P[i].ID;

    mpsort_mpi(ids, NumPart, sizeof(MyIDType), radix_id, 8, NULL, MPI_COMM_WORLD);

    for(i = 1; i < NumPart; i++)
        if(ids[i] == ids[i - 1])
        {
            endrun(12, "non-unique ID=%013ld found on task=%d (i=%d NumPart=%d)\n",
                    ids[i], ThisTask, i, NumPart);

        }

    MPI_Allgather(&ids[0], sizeof(MyIDType), MPI_BYTE, ids_first, sizeof(MyIDType), MPI_BYTE, MPI_COMM_WORLD);

    if(ThisTask < NTask - 1)
        if(ids[NumPart - 1] == ids_first[ThisTask + 1])
        {
            endrun(13, "non-unique ID=%d found on task=%d\n", (int) ids[NumPart - 1], ThisTask);
        }

    myfree(ids_first);
    myfree(ids);

    t1 = second();

    message(0, "success.  took=%g sec\n", timediff(t0, t1));
}
