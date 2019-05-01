#ifndef FOF_H
#define FOF_H

#include "forcetree.h"
#include "utils/paramset.h"

void set_fof_params(ParameterSet * ps);

void fof_init(double DMMeanSeparation);

/*Computes the Group structure, saved as a global array below*/
void fof_fof(ForceTree * tree, double BoxSize);

/*Frees the Group structure*/
void
fof_finish(void);

/*Uses the Group structure to seed blackholes*/
void
fof_seed(void);

/*Saves the Group structure to disc.*/
void
fof_save_groups(int num);

extern int Ngroups;
extern int64_t TotNgroups;

struct BaseGroup {
    int OriginalTask;
    int OriginalIndex;
    int Length;
    int GrNr;
    MyIDType MinID;
    int MinIDTask;
    float FirstPos[3];
};

extern struct Group
{
    struct BaseGroup base;
    int Length;
    int LenType[6];
    double MassType[6];
    double Mass;
    double CM[3];
    double Vel[3];

    double Imom[3][3]; /* sum M r_j r_k */
    double Jmom[3]; /* sum M R_i x V_i  */

    double Sfr;
#ifdef BLACK_HOLES
    double BH_Mass;
    double BH_Mdot;
    double MaxDens;
#endif
    int seed_index;
    int seed_task;
} * Group;

#endif
