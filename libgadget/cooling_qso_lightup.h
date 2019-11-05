#ifndef _COOLING_QSO_LIGHTUP_H
#define _COOLING_QSO_LIGHTUP_H

#include "forcetree.h"
#include "fof.h"
#include "utils/paramset.h"

void set_qso_lightup_params(ParameterSet * ps);

/* Initialize the helium reionization cooling module*/
void init_qso_lightup(char * reion_hist_file);

/* Starts reionization by selecting the first halo and flagging all particles in the first HeIII bubble*/
void do_heiii_reionization(double redshift, FOFGroups * fof, ForceTree * tree);

/* Get the long mean free path photon heating that applies to not-yet-ionized particles*/
double get_long_mean_free_path_heating(double redshift);


#endif
