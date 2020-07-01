#include "ClosureManager.hpp"

using namespace std;
using namespace chrono;


ClosureManager::ClosureManager(std::shared_ptr<Loader> ldr,
                               std::shared_ptr<GridManager> gridMnr):loader(move(ldr)),
                                                                gridMgr(move(gridMnr)){
    logger.reset(new Logger());
    initialize();
    logger->writeMsg("[ClosureManager] create...OK", DEBUG);
}

ClosureManager::~ClosureManager(){
    // need for driver calculation
    delete driverNext;
    delete pressuNext;
    delete electrnVel;

    delete bfieldPrev;
    
}

void ClosureManager::initialize(){
    int xRes = loader->resolution[0],
        yRes = loader->resolution[1],
        zRes = loader->resolution[2];
    int xResG2 = xRes+2, yResG2 = yRes+2, zResG2 = zRes+2;
    
    int nG2= xResG2*yResG2*zResG2;
    
    driverNext = new double[nG2*6*sizeof(double)];
    pressuNext = new double[nG2*6*sizeof(double)];
    electrnVel = new double[nG2*3*sizeof(double)];
    
    emass = loader->electronmass;
    
    initPressure();
    
    int h, ijkG1, ijkG2;
   
    VectorVar** pressure = gridMgr->getVectorVariableOnG2(PRESSURE);
    
    for (ijkG2 = 0; ijkG2 < nG2; ijkG2++) {
        for (h = 0; h < 6; h++) {
            double pres = pressure[ijkG2]->getValue()[h];
            pressuNext[6*ijkG2+h] = pres;
            driverNext[6*ijkG2+h] = pres;
        }
    }

    int xResG1 = xRes+1, yResG1 = yRes+1, zResG1 = zRes+1;
    
    int nG1= xResG1*yResG1*zResG1;
    
    bfieldPrev = new double[nG1*3*sizeof(double)];
    
    VectorVar** bFieldKeep = gridMgr->getVectorVariableOnG1(MAGNETIC);
    for (ijkG1 = 0; ijkG1 < nG1; ijkG1++) {
        for (h = 0; h < 3; h++) {
            bfieldPrev[3*ijkG1+h] = bFieldKeep[ijkG1]->getValue()[h];
        }
    }
    
    calculatePressure(PREDICTOR, -1);
    calculatePressure(CORRECTOR, -1);
}


void ClosureManager::initPressure(){
    
    double pres;
    
    int idx, idxOnG2;
    
    double domainShiftX = loader->boxCoordinates[0][0];
    double domainShiftY = loader->boxCoordinates[1][0];
    double domainShiftZ = loader->boxCoordinates[2][0];
    
    double dx = loader->spatialSteps[0];
    double dy = loader->spatialSteps[1];
    double dz = loader->spatialSteps[2];
    
    int xRes = loader->resolution[0];
    int yRes = loader->resolution[1];
    int zRes = loader->resolution[2];
    int xResG2 = xRes+2, yResG2 = yRes+2, zResG2 = zRes+2;
    double G2shift = 0.5;
    double x,y,z;
    int i,j,k;
    for( i = 0; i < xRes; i++){
        for( j = 0; j < yRes; j++) {
            for( k = 0; k < zRes; k++){
                
                idxOnG2 = IDX(i+1, j+1, k+1, xResG2, yResG2, zResG2);
                
                x = (i + G2shift)*dx + domainShiftX;
                y = (j + G2shift)*dy + domainShiftY;
                z = (k + G2shift)*dz + domainShiftZ;
        
                pres = loader->getElectronPressure(x,y,z);
        
                gridMgr->setVectorVariableForNodeG2(idxOnG2, PRESSURE, 0, pres);
                gridMgr->setVectorVariableForNodeG2(idxOnG2, PRESSURE, 3, pres);
                gridMgr->setVectorVariableForNodeG2(idxOnG2, PRESSURE, 5, pres);
                
                gridMgr->setVectorVariableForNodeG2(idxOnG2, PRESSURE_AUX, 0, pres);
                gridMgr->setVectorVariableForNodeG2(idxOnG2, PRESSURE_AUX, 3, pres);
                gridMgr->setVectorVariableForNodeG2(idxOnG2, PRESSURE_AUX, 5, pres);
        
                gridMgr->setVectorVariableForNodeG2(idxOnG2, DRIVER, 0, pres);
                gridMgr->setVectorVariableForNodeG2(idxOnG2, DRIVER, 3, pres);
                gridMgr->setVectorVariableForNodeG2(idxOnG2, DRIVER, 5, pres);
                
            }
        }
    }
    
    gridMgr->sendBoundary2Neighbor(PRESSURE);
    gridMgr->sendBoundary2Neighbor(PRESSURE_AUX);
    gridMgr->sendBoundary2Neighbor(DRIVER);
    
    gridMgr->applyBC(PRESSURE);
    gridMgr->applyBC(PRESSURE_AUX);
    gridMgr->applyBC(DRIVER);
    
    VectorVar** pdriverr = gridMgr->getVectorVariableOnG2(DRIVER);
    int nG2= xResG2*yResG2*zResG2;
    for (idxOnG2 = 0; idxOnG2 < nG2; idxOnG2++) {
        for (int h = 0; h < 6; h++) {
            const double* dr = pdriverr[idxOnG2]->getValue();
            gridMgr->setVectorVariableForNodeG2(idxOnG2, DRIVER_AUX, h, dr[h]);
        }
    }
}


void ClosureManager::calculatePressure(int phase, int i_time){
    subCycledPressure(phase, i_time) ;
}


void ClosureManager::setIsotropization(double pres[6], double iTerm[6]) {
    
    double trP = (pres[0]+pres[3]+pres[5])/3;
    double buf = loader->relaxFactor;
    
    iTerm[0] = -buf*(pres[0] - trP);
    iTerm[1] = -buf*pres[1];
    iTerm[2] = -buf*pres[2];
    iTerm[3] = -buf*(pres[3] - trP);
    iTerm[4] = -buf*pres[4];
    iTerm[5] = -buf*(pres[5] - trP);
}



void ClosureManager::subCycledPressure(int phase, int i_time) {
    
    auto start_time = high_resolution_clock::now();
    
    int xRes = loader->resolution[0];
    int yRes = loader->resolution[1];
    int zRes = loader->resolution[2];
    int xResG2 = xRes+2, yResG2 = yRes+2, zResG2 = zRes+2;
    int xResG1 = xRes+1, yResG1 = yRes+1, zResG1 = zRes+1;

    int nG2 = xResG2*yResG2*zResG2;
    int nG1 = xResG1*yResG1*zResG1;

    int ijkG2, ijkG1, h, i, j, k, m;
    
    double pSub[6];
    double cTerm[6];
    double iTerm[6];
    
    double omega;
    
    double vecB[3];
    double vecBnext[3];
    double unitB[3];
    double modulusB;
    
    double *vecBstartAll = new double[nG2*3*sizeof(double)];
    double *vecBstepAll  = new double[nG2*3*sizeof(double)];
    
    double *pSubAll      = new double[nG2*6*sizeof(double)];
    double *iTermAll     = new double[nG2*6*sizeof(double)];;
    
    double ts = loader->getTimeStep();
    const double subDt = ts*emass;
    
    const int numOfSubStep = (int)(1.0/emass);
    
    string msg ="[ClosureManager] start to calculate Pressure subDt = "
                    +to_string(subDt)+" numOfSubStep = "+to_string(numOfSubStep);
    logger->writeMsg(msg.c_str(), DEBUG);

    VectorVar** pressure    = gridMgr->getVectorVariableOnG2(PRESSURE);
    VectorVar** pressureaux = gridMgr->getVectorVariableOnG2(PRESSURE_AUX);
    VectorVar** pdriverr    = gridMgr->getVectorVariableOnG2(DRIVER);
    
    int magField2use;
    switch (phase){
        case PREDICTOR:
            magField2use = MAGNETIC_AUX;
            
            for(ijkG2=0; ijkG2<nG2; ijkG2++){
                for (h = 0; h < 6; h++) {
                    pSubAll[ijkG2*6+h] = pressureaux[ijkG2]->getValue()[h];
                }
            }
            break;
        case CORRECTOR:
            magField2use = MAGNETIC;
            
            for(ijkG2=0; ijkG2<nG2; ijkG2++){
                for (h = 0; h < 6; h++) {
                    pSubAll[ijkG2*6+h] = pressure[ijkG2]->getValue()[h];
                }
            }
            break;
        default :
            throw runtime_error("no phase");
    }
    
    const int* neighbourhood = gridMgr->getNeighbourhoodOnG1();
    VectorVar** bField = gridMgr->getVectorVariableOnG1(magField2use);
    
    int neighbour, idxNeigbor;
    
    //precalculations
    for (i = 1; i < xRes + 1; i++) {
        for (j = 1; j < yRes + 1; j++) {
            for (k = 1; k < zRes + 1; k++) {
                
                for (h = 0; h < 3; h++) {
                    vecBnext[h] = 0.0;
                    vecB[h]     = 0.0;
                }
                
                ijkG1 = IDX(i, j, k, xResG1, yResG1, zResG1);
                
                for (neighbour=0; neighbour<8; neighbour++){
                    idxNeigbor = neighbourhood[8*ijkG1+neighbour];
                    for (h = 0; h < 3; h++) {
                        vecBnext[h] += 0.125*bField[idxNeigbor]->getValue()[h];
                        vecB[h]     += 0.125*bfieldPrev[3*idxNeigbor+h];
                    }
                }
                
                ijkG2 = IDX(i, j, k, xResG2, yResG2, zResG2);
                
                for (h = 0; h < 3; h++) {
                    vecBstartAll[ijkG2*3+h] = vecB[h];
                    vecBstepAll [ijkG2*3+h] = (vecBnext[h]-vecB[h])/numOfSubStep;
                }
                
                for (h = 0; h < 6; h++) {
                    pSub[h] = pSubAll[ijkG2*6+h];
                }
                
                setIsotropization(pSub, iTerm);
                
                for (h = 0; h < 6; h++) {
                    iTermAll[ijkG2*6+h] = iTerm[h];
                }
                
            }
        }
    }
    
    auto end_time1 = high_resolution_clock::now();
    string msg1 ="[ClosureManager] calculatePressure(): precalculations duration = "+
                    to_string(duration_cast<milliseconds>(end_time1 - start_time).count())+" ms";
    logger->writeMsg(msg1.c_str(), DEBUG);
    
    
    
    for (i = 1; i < xRes + 1; i++) {
        for (j = 1; j < yRes + 1; j++) {
            for (k = 1; k < zRes + 1; k++) {
                    
                ijkG2 = IDX(i, j, k, xResG2, yResG2, zResG2);

                for (h = 0; h < 6; h++) {
                    pSub[h] = pSubAll[ijkG2*6+h];
                }
                
                const double* dr = pdriverr[ijkG2]->getValue();
                
                for (m = 0; m < numOfSubStep; m++) {
                    
                    for (h = 0; h < 6; h++) {
                        cTerm[h] = 0.0;
                    }
                    
                    for (h = 0; h < 3; h++) {
                        vecB[h] = vecBstartAll[ijkG2*3+h] + m*vecBstepAll[ijkG2*3+h];
                        unitB[h] = 0.0;
                    }
                    
                    modulusB = sqrt(vecB[0]*vecB[0]+vecB[1]*vecB[1]+vecB[2]*vecB[2]);
                    
                    omega = 0.0;
                    
                    if (modulusB > EPS8) {
                        
                        for (int h = 0; h < 3; h++) {
                            unitB[h] = vecB[h]/modulusB;
                        }
                        omega = modulusB/emass;
                        
                        cTerm[0] = -( 2.0*(pSub[1]*unitB[2]-pSub[2]*unitB[1]) );
                        
                        cTerm[1] = -( pSub[2]*unitB[0]-pSub[0]*unitB[2]
                                     +pSub[3]*unitB[2]-pSub[4]*unitB[1] );
                        
                        cTerm[2] = -( pSub[0]*unitB[1]-pSub[1]*unitB[0]
                                     +pSub[4]*unitB[2]-pSub[5]*unitB[1] );
                        
                        cTerm[3] = -( 2.0*(pSub[4]*unitB[0]-pSub[1]*unitB[2]) );
                        
                        cTerm[4] = -( pSub[1]*unitB[1]-pSub[3]*unitB[0]
                                     +pSub[5]*unitB[0]-pSub[2]*unitB[2] );
                        
                        cTerm[5] = -( 2.0*(pSub[2]*unitB[1]-pSub[4]*unitB[0]) );
                    }
                    
                    for (h = 0; h < 6; h++) {
                        pSub[h] += subDt*( dr[h] + omega*cTerm[h] + iTermAll[ijkG2*6+h]);
                        
#ifdef LOG
                        if (std::isnan(pSub[h]) || abs(pSub[h]) > BIGN){
                            for (int hh = 0; hh < 6; hh++) {
                                string err ="[ClosureManager] pSub["+to_string(hh)+"] = "+
                                to_string(pSub[hh])+" dr = "+to_string(dr[hh])
                                +"\n    cTerm = "+to_string(cTerm[hh])
                                +"\n    iTermAll = "+to_string(iTermAll[ijkG2*6+hh])
                                +"\n    i = "+to_string(i)
                                +"\n    j = "+to_string(j)
                                +"\n    k = "+to_string(k)
                                +"\n    Xshift = "+to_string(loader->boxCoordinates[0][0])
                                +"\n    Yshift = "+to_string(loader->boxCoordinates[1][0])
                                +"\n    Zshift = "+to_string(loader->boxCoordinates[2][0]);
                                logger->writeMsg(err.c_str(), CRITICAL);
                            }
                        }
#endif
                        
                    }
                 }
                
                for (h = 0; h < 6; h++) {
                        pSubAll[ijkG2*6+h] = pSub[h];
                }
                
            }
        }
    }
    
    auto end_time2 = high_resolution_clock::now();
    string msg212 ="[ClosureManager] calculatePressure() subcycled duration = "+
    to_string(duration_cast<milliseconds>(end_time2 - end_time1).count())+" ms";
    logger->writeMsg(msg212.c_str(), DEBUG);
    
    
    for (i = 1; i < xRes + 1; i++) {
        for (j = 1; j < yRes + 1; j++) {
            for (k = 1; k < zRes + 1; k++) {
                ijkG2 = IDX(i, j, k, xResG2, yResG2, zResG2);
                for (h = 0; h < 6; h++) {
                     gridMgr->setVectorVariableForNodeG2(ijkG2, PRESSURE, h, pSubAll[ijkG2*6+h]);
                }
            }
        }
    }
    
    delete vecBstartAll;
    delete vecBstepAll;
    delete pSubAll;
    delete iTermAll;
    
    
    gridMgr->sendBoundary2Neighbor(PRESSURE);
    gridMgr->applyBC(PRESSURE);
    
    if(i_time % loader->smoothStride == 0){
        gridMgr->smooth(PRESSURE);
        gridMgr->applyBC(PRESSURE);
    }
    
    pressure = gridMgr->getVectorVariableOnG2(PRESSURE);
    
    for (ijkG2 = 0; ijkG2 < nG2; ijkG2++) {
        for (h = 0; h < 6; h++) {
            pressuNext[6*ijkG2+h] = pressure[ijkG2]->getValue()[h];
        }
    }
    
    if (phase == PREDICTOR) {
        for (ijkG2 = 0; ijkG2 < nG2; ijkG2++) {
            for (h = 0; h < 6; h++) {
                gridMgr->setVectorVariableForNodeG2(ijkG2, PRESSURE_AUX, h,
                                                    pressure[ijkG2]->getValue()[h]);
            }
        }
        
        VectorVar** bFieldKeep = gridMgr->getVectorVariableOnG1(MAGNETIC_AUX);
        for (ijkG1 = 0; ijkG1 < nG1; ijkG1++) {
            for (h = 0; h < 3; h++) {
                bfieldPrev[3*ijkG1+h] = bFieldKeep[ijkG1]->getValue()[h];
            }
        }
    }

    auto end_time3 = high_resolution_clock::now();
    string msg23 ="[ClosureManager] calculatePressure() set values duration = "+
    to_string(duration_cast<milliseconds>(end_time3 - end_time2).count())+" ms";
    logger->writeMsg(msg23.c_str(), DEBUG);
    
    setDriver(phase);
    
    
    auto end_time4 = high_resolution_clock::now();
    string msg22 ="[ClosureManager] calculatePressure() set driver duration = "+
    to_string(duration_cast<milliseconds>(end_time4 - end_time3).count())+" ms";
    logger->writeMsg(msg22.c_str(), DEBUG);
    
}


void ClosureManager::setDriver(int phase){
    
    int xRes = loader->resolution[0],
        yRes = loader->resolution[1],
        zRes = loader->resolution[2];
    int xResG2 = xRes+2, yResG2 = yRes+2, zResG2 = zRes+2;
    int nG2 = xResG2*yResG2*zResG2;
    
    int ijkG2;
    
    int i, j, k, l, m;
    int idx3D[3];
    int h;
    double dTerms[3][3];
    
    double *pDrive = new double[nG2*6*sizeof(double)];
    
    VectorVar** current = gridMgr->getVectorVariableOnG2(CURRENT);
    
    for(ijkG2=0; ijkG2<nG2; ijkG2++){
        for (h = 0; h < 3; h++) {
            gridMgr->setVectorVariableForNodeG2(ijkG2, CURRENT_AUX, h,
                                                current[ijkG2]->getValue()[h]);
        }
    }
    
    gridMgr->smooth(CURRENT_AUX);
    gridMgr->applyBC(CURRENT_AUX);
    
    VectorVar** current_aux = gridMgr->getVectorVariableOnG2(CURRENT_AUX);
    
    VectorVar** density  = gridMgr->getVectorVariableOnG2(DENSELEC);
    VectorVar** velocity = gridMgr->getVectorVariableOnG2(VELOCION);
    const double* jcur;
    const double* nele;
    const double* vion;
    for (ijkG2 = 0; ijkG2 < nG2; ijkG2++) {
        jcur = current_aux[ijkG2]->getValue();
        nele = density[ijkG2]->getValue();
        vion = velocity[ijkG2]->getValue();
        
        for (l = 0; l < 3; l++) {
           electrnVel[3*ijkG2+l] = vion[l]-(jcur[l]/nele[0]);
        }
    }
    
    double pe[3][3];
    double nabV[3][3];
    double nabP[3][3][3];
    int n;
    
    for (i = 1; i < xRes + 1; i++) {
        for (j = 1; j < yRes + 1; j++) {
            for (k = 1; k < zRes + 1; k++) {
                
                ijkG2 = IDX(i, j, k, xResG2, yResG2, zResG2);
                
                idx3D[0] = i;
                idx3D[1] = j;
                idx3D[2] = k;
                
                gradients(pe, nabV, nabP, idx3D);
                
                double divV = nabV[0][0]+nabV[1][1]+nabV[2][2];
                
                for (l = 0; l < 3; l++) {
                    for (m = l; m < 3; m++) {
                        
                        /* __ P nabla . V __ */
                        dTerms[l][m] = -pe[l][m]*divV;
                        
                        for (n = 0; n < 3; n++) {
                            /* __ V . nabla P __ */
                            dTerms[l][m] -= electrnVel[3*ijkG2+n]*nabP[n][l][m];
                            /* __ P . nabla V __ */
                            dTerms[l][m] -= pe[l][n]*nabV[n][m];
                            /* __ P . nabla V (transposed) __ */
                            dTerms[l][m] -= pe[m][n]*nabV[n][l];
                        }
                        /* __ & set symmetrical terms __ */
                        dTerms[m][l] = dTerms[l][m];

#ifdef LOG
                        if(std::isnan(dTerms[m][l]) || abs(dTerms[m][l]) > BIGN){
                                string err ="[ClosureManager] dTerms["+to_string(m)+"]["+to_string(l)+"] = "
                                +to_string(dTerms[m][l])+"\n   divV = "+to_string(divV)
                                +to_string(dTerms[m][l])+"\n   nabV[0][0] = "+to_string(nabV[0][0])
                                +to_string(dTerms[m][l])+"\n   nabV[1][1] = "+to_string(nabV[1][1])
                                +to_string(dTerms[m][l])+"\n   nabV[2][2] = "+to_string(nabV[2][2])
                                +"\n     -pe[l][m]*divVcTerm = "+to_string( -pe[l][m]*divV)
                                +"\n    electrnVel[3*ijkG2+0]*nabP[0][l][m] = "+to_string(electrnVel[3*ijkG2+0]*nabP[0][l][m])
                                +"\n    electrnVel[3*ijkG2+1]*nabP[1][l][m] = "+to_string(electrnVel[3*ijkG2+1]*nabP[1][l][m])
                                +"\n    electrnVel[3*ijkG2+2]*nabP[2][l][m] = "+to_string(electrnVel[3*ijkG2+2]*nabP[2][l][m])
                                +"\n    pe[l][0]*nabV[0][m] = "+to_string(pe[l][0]*nabV[0][m])
                                +"\n    pe[l][1]*nabV[1][m] = "+to_string(pe[l][1]*nabV[1][m])
                                +"\n    pe[l][2]*nabV[2][m] = "+to_string(pe[l][2]*nabV[2][m])
                                +"\n    pe[m][n]*nabV[0][l] = "+to_string(pe[m][0]*nabV[0][l])
                                +"\n    pe[m][n]*nabV[1][l] = "+to_string(pe[m][1]*nabV[1][l])
                                +"\n    pe[m][n]*nabV[2][l] = "+to_string(pe[m][2]*nabV[2][l])
                                +"\n    i = "+to_string(i)
                                +"\n    j = "+to_string(j)
                                +"\n    k = "+to_string(k);
                                logger->writeMsg(err.c_str(), CRITICAL);
                        }
#endif
                    }
                }
                
                h = 0;
                for (l = 0; l < 3; l++) {
                    for (m = l; m < 3; m++) {
                        pDrive[ijkG2*6+h] = dTerms[l][m];
                        h++;
                    }
                }
            }
        }
    }
    
    VectorVar** driveaux = gridMgr->getVectorVariableOnG2(DRIVER_AUX);
    
    switch (phase) {
        case PREDICTOR:
            for(ijkG2=0; ijkG2<nG2; ijkG2++){
                for (h = 0; h < 6; h++) {
                    driverNext[6*ijkG2+h] = -driverNext[6*ijkG2+h]+2.0*pDrive[ijkG2*6+h];
                    gridMgr->setVectorVariableForNodeG2(ijkG2, DRIVER    , h, driverNext[ijkG2*6+h]);
                    gridMgr->setVectorVariableForNodeG2(ijkG2, DRIVER_AUX, h, pDrive[ijkG2*6+h]);
                }
            }
            gridMgr->sendBoundary2Neighbor(DRIVER_AUX);
            gridMgr->applyBC(DRIVER_AUX);
            break;
        case CORRECTOR:
            for(ijkG2=0; ijkG2<nG2; ijkG2++){
                for (h = 0; h < 6; h++) {
                    driverNext[6*ijkG2+h] = 0.5*(pDrive[ijkG2*6+h]+driveaux[ijkG2]->getValue()[h]);
                    gridMgr->setVectorVariableForNodeG2(ijkG2, DRIVER, h, driverNext[ijkG2*6+h]);
                }
            }
            break;
        default:
            throw runtime_error("no phase");
    }
    
    gridMgr->sendBoundary2Neighbor(DRIVER);
    gridMgr->applyBC(DRIVER);
    
    delete pDrive;
}


void ClosureManager::gradients(double pe[3][3], double nabV[3][3],
                               double nabP[3][3][3], int idx3D[3]) {
    
    int xRes = loader->resolution[0];
    int yRes = loader->resolution[1];
    int zRes = loader->resolution[2];
    int xResG2 = xRes+2, yResG2 = yRes+2, zResG2 = zRes+2;
    
    float upwindIDX[3][3];
    float sign;
    int diffIDX[3][2];
    int ijkG2, h, l, m, s;
    
    double dx = loader->spatialSteps[0];
    double dy = loader->spatialSteps[1];
    double dz = loader->spatialSteps[2];
    double dl[3] = {dx, dy, dz};
    
    
    
    int zeroOrderNeighb[6][3]  =
       {{-1, 0, 0}, {+1, 0, 0},
        { 0,-1, 0}, { 0,+1, 0},
        { 0, 0,-1}, { 0, 0,+1}};
    
    int firstOrderNeighb[24][3] =
       {{-1,-1, 0}, {+1,-1, 0},
        {-1,+1, 0}, {+1,+1, 0},
        {-1, 0,-1}, {+1, 0,-1},
        {-1, 0,+1}, {+1, 0,+1},
           
        {-1,-1, 0}, {-1,+1, 0},
        {+1,-1, 0}, {+1,+1, 0},
        { 0,-1,-1}, { 0,+1,-1},
        { 0,-1,+1}, { 0,+1,+1},
       
       {-1, 0,-1}, {-1, 0,+1},
       {+1, 0,-1}, {+1, 0,+1},
       { 0,-1,-1}, { 0,-1,+1},
       { 0,+1,-1}, { 0,+1,+1}};
    
    /* __ indices to make difference in each dir {x,y,z} __ */
    diffIDX[0][0] = IDX(idx3D[0]+1, idx3D[1]  , idx3D[2]  , xResG2, yResG2, zResG2);
    diffIDX[0][1] = IDX(idx3D[0]-1, idx3D[1]  , idx3D[2]  , xResG2, yResG2, zResG2);
    diffIDX[1][0] = IDX(idx3D[0]  , idx3D[1]+1, idx3D[2]  , xResG2, yResG2, zResG2);
    diffIDX[1][1] = IDX(idx3D[0]  , idx3D[1]-1, idx3D[2]  , xResG2, yResG2, zResG2);
    diffIDX[2][0] = IDX(idx3D[0]  , idx3D[1]  , idx3D[2]+1, xResG2, yResG2, zResG2);
    diffIDX[2][1] = IDX(idx3D[0]  , idx3D[1]  , idx3D[2]-1, xResG2, yResG2, zResG2);
    
    
    const double k1 = 0.250;// 1/4
    const double k2 = 0.0625;// 1/8
    int idxRight, idxLeft;
    
    /* __  nabla V centered in space __ */
    for (l = 0; l < 3; l++) {//d{x,y,z}
        for (m = 0; m < 3; m++) {//Vele{x,y,z}
            
            idxRight = IDX(idx3D[0]+zeroOrderNeighb[2*l+1][0],
                           idx3D[1]+zeroOrderNeighb[2*l+1][1],
                           idx3D[2]+zeroOrderNeighb[2*l+1][2],
                           xResG2,yResG2,zResG2);
            
            idxLeft =  IDX(idx3D[0]+zeroOrderNeighb[2*l+0][0],
                           idx3D[1]+zeroOrderNeighb[2*l+0][1],
                           idx3D[2]+zeroOrderNeighb[2*l+0][2],
                           xResG2,yResG2,zResG2);

            

            nabV[l][m] = k1*(electrnVel[3*idxRight+m] - electrnVel[3*idxLeft+m]);
            
            for (int pairNum = 0; pairNum<4; pairNum++){
                idxRight = IDX(idx3D[0]+firstOrderNeighb[8*l+2*pairNum+1][0],
                               idx3D[1]+firstOrderNeighb[8*l+2*pairNum+1][1],
                               idx3D[2]+firstOrderNeighb[8*l+2*pairNum+1][2],
                               xResG2,yResG2,zResG2);
                
                idxLeft  = IDX(idx3D[0]+firstOrderNeighb[8*l+2*pairNum+0][0],
                               idx3D[1]+firstOrderNeighb[8*l+2*pairNum+0][1],
                               idx3D[2]+firstOrderNeighb[8*l+2*pairNum+0][2],
                               xResG2,yResG2,zResG2);
                
                nabV[l][m] += k2*(electrnVel[3*idxRight+m] - electrnVel[3*idxLeft+m]);
            }
            
        
            nabV[l][m] = nabV[l][m]/dl[l];

        }
    }
    
    ijkG2 = IDX(idx3D[0], idx3D[1], idx3D[2], xResG2, yResG2, zResG2);
    
    /* __ upwind coefficients defined by flow direction __ */
    for (l = 0; l < 3; l++) {//Vele{x,y,z}
  
        sign = copysignf(1, electrnVel[3*ijkG2+l]);
        upwindIDX[l][0] = +0.5*(1.0-sign)/dl[l]; //sign=-1 - counterflow direction use (Pi-1 - Pi)
        upwindIDX[l][1] =           sign /dl[l]; //center
        upwindIDX[l][2] = -0.5*(1.0+sign)/dl[l]; //sign=+1 - flow direction use (Pi - Pi+1)
    }
    

    h = 0;
    /* __  nabla P upwind scheme __ */
    for (l = 0; l < 3; l++) {
        for (m = l; m < 3; m++) {
            
            pe[l][m] = pressuNext[6*ijkG2+h];
            pe[m][l] = pe[l][m];
            
            for (s = 0; s < 3; s++) {//d{x,y,z}
                nabP[s][l][m] = upwindIDX[s][0]*pressuNext[6*diffIDX[s][0]+h]
                               +upwindIDX[s][1]*pe[l][m]
                               +upwindIDX[s][2]*pressuNext[6*diffIDX[s][1]+h];
                
                nabP[s][m][l] = nabP[s][l][m];// by Pij symmetry
            }
            
            h++;
        }
    }
}


