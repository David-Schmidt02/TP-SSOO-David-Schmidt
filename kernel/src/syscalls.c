#include <main.h>
#include <pcb.h>
#include <syscalls.h>
#include "planificador_corto_plazo.h"
#include "planificador_largo_plazo.h"

extern t_list* lista_global_tcb;  // Lista global de TCBs

extern t_tcb* hilo_actual;          
extern t_pcb* proceso_actual;

//Estructuras provenientes del planificador_largo_plazo
extern t_cola_proceso* procesos_cola_ready;
extern pthread_mutex_t * mutex_procesos_cola_ready;
extern sem_t * sem_estado_procesos_cola_ready;

extern t_cola_hilo* hilos_cola_exit;
extern pthread_mutex_t * mutex_hilos_cola_exit;
extern sem_t * sem_estado_hilos_cola_exit;

extern t_cola_procesos_a_crear* procesos_a_crear;
extern pthread_mutex_t * mutex_procesos_a_crear;
extern sem_t * sem_estado_procesos_a_crear;
//

//Estructuras provenientes del planificador_corto_plazo
extern t_cola_hilo* hilos_cola_ready;
extern pthread_mutex_t * mutex_hilos_cola_ready;
extern sem_t * sem_estado_hilos_cola_ready;

extern t_cola_hilo* hilos_cola_bloqueados;
extern pthread_mutex_t * mutex_hilos_cola_bloqueados;
extern sem_t * sem_estado_hilos_cola_bloqueados;

extern t_colas_multinivel *colas_multinivel;
extern pthread_mutex_t * mutex_colas_multinivel;
extern sem_t * sem_estado_multinivel;

extern char* algoritmo;
//

extern t_list* lista_mutexes; // Lista global de mutexes
extern t_list* lista_procesos; // Lista global de procesos
extern int ultimo_tid;
//

extern sem_t * sem_estado_respuesta_desde_memoria;

extern t_cola_IO *colaIO;
extern pthread_mutex_t * mutex_colaIO;
extern sem_t * sem_estado_colaIO;

t_pcb* obtener_pcb_por_tid(int tid) {
    for (int i = 0; i < list_size(procesos_cola_ready->lista_procesos); i++) {
        t_pcb* pcb = list_get(procesos_cola_ready->lista_procesos, i);
        for (int j = 0; j < list_size(pcb->listaTCB); j++) {
            t_tcb* tcb_actual = list_get(pcb->listaTCB, j);
            if (tcb_actual->tid == tid) {
                return pcb;
            }
        }
    }
    return NULL;
}

t_pcb* obtener_pcb_por_pid(int pid) {
    for (int i = 0; i < list_size(procesos_cola_ready->lista_procesos); i++) {
        t_pcb* pcb = list_get(procesos_cola_ready->lista_procesos, i);
        if (pcb->pid== pid)
            return pcb;
    }
    return NULL;
}

//PROCESOS

void PROCESS_CREATE(FILE* archivo_instrucciones, int tam_proceso, int prioridadTID) {
    int pid = generar_pid_unico();
    int pc = 0;

    t_pcb* nuevo_pcb = crear_pcb(pid, pc, prioridadTID);
    nuevo_pcb->memoria_necesaria = tam_proceso;
    nuevo_pcb->quantum = prioridadTID;

    log_info(logger, "Creación de Proceso: ## (<pid>:<%d>) Se crea el Proceso - Estado: NEW", nuevo_pcb->pid);

    t_tcb* tcb_principal = crear_tcb(pid, 0, prioridadTID);
    tcb_principal->estado = NEW;

    log_info(logger, "Creación del hilo principal para el proceso PID: %d", nuevo_pcb->pid);

    nuevo_pcb->listaTCB = list_create();
    nuevo_pcb->listaMUTEX = list_create();

    list_add(nuevo_pcb->listaTCB, tcb_principal);
    
    t_list* lista_instrucciones = interpretarArchivo(archivo_instrucciones);
    
    tcb_principal->instrucciones = lista_instrucciones;
    
    pthread_mutex_lock(mutex_procesos_a_crear);
    list_add(procesos_a_crear->lista_procesos, nuevo_pcb);
    sem_post(sem_estado_procesos_a_crear);
    pthread_mutex_unlock(mutex_procesos_a_crear);

    log_info(logger, "Proceso PID %d agregado a la lista de procesos.", nuevo_pcb->pid);
    
    liberarInstrucciones(lista_instrucciones);

    log_info(logger, "## (<PID>:<TID>) - Solicitó syscall: PROCESS_CREATE");
    
}

void eliminar_mutex(t_mutex* mutex) {
    if (mutex != NULL) {
        if (mutex->hilos_esperando != NULL) {
            list_destroy(mutex->hilos_esperando);
        }
        free(mutex->nombre);
        free(mutex);
    }
}

void eliminar_pcb(t_pcb* pcb) {
    if (pcb->listaTCB != NULL) {
        list_destroy_and_destroy_elements(pcb->listaTCB, (void*) eliminar_tcb);
    }

    if (pcb->listaMUTEX != NULL) {
        list_destroy_and_destroy_elements(pcb->listaMUTEX, (void*) eliminar_mutex); // <-- Asegúrate de tener esta función
    }

    if (pcb->registro != NULL) {
        free(pcb->registro);
    }

    free(pcb);

    log_info(logger, "PCB eliminado exitosamente.");
}

void PROCESS_EXIT() {
    // Usamos el pid del hilo actual
    int pid_buscado = hilo_actual->pid;
    t_pcb* pcb_encontrado = NULL;

    // Buscar el PCB que coincide con el pid del hilo actual
    for (int i = 0; i < list_size(procesos_cola_ready->lista_procesos); i++) {
        t_pcb* pcb = list_get(procesos_cola_ready->lista_procesos, i);
        if (pid_buscado == pcb->pid) {
            pcb_encontrado = pcb;
            break;
        }
    }
    // Validar si se encontró el PCB
    if (pcb_encontrado == NULL) {
        log_warning(logger, "No se encontró el PCB para el proceso con PID: %d", pid_buscado);
        return;
    }

    log_info(logger, "Finalizando el proceso con PID: %d", pcb_encontrado->pid);

    // Cambiar el estado de todos los TCBs asociados a EXIT
    for (int i = 0; i < list_size(pcb_encontrado->listaTCB); i++) {
        t_tcb* tcb_asociado = list_get(pcb_encontrado->listaTCB, i);
        cambiar_estado(tcb_asociado, EXIT);
        //acá hay que hacer dos cosas, eliminarlos de la cola de hilos y moverlos a una cola de exit
        if (strcmp(algoritmo, "FIFO") == 0 || strcmp(algoritmo, "PRIORIDADES")) {
            eliminar_hilo_de_cola_fifo_prioridades(tcb_asociado);
        } else if (strcmp(algoritmo, "CMN") == 0) {
            eliminar_hilo_de_cola_multinivel(tcb_asociado);
        } else {
            printf("Error: Algoritmo no reconocido.\n");
        }
        log_info(logger, "TCB TID: %d del proceso PID: %d ha cambiado a estado EXIT", tcb_asociado->tid, pcb_encontrado->pid);
    }
    log_info(logger, "Todos los hilos del proceso %d eliminados de READY.", pid_buscado);

    // Notificar a la memoria que el proceso ha finalizado
    notificar_memoria_fin_proceso(pcb_encontrado->pid);
    usleep(1);

    // Eliminar el PCB de la lista de procesos
    pthread_mutex_lock(mutex_procesos_cola_ready);
    // linea anterior porque no estoy seguro de como funciona//
    //list_remove_and_destroy_by_condition(procesos_cola_ready->lista_procesos, (void*) (pcb_encontrado->pid == pid_buscado), (void*) eliminar_pcb);
    list_remove_and_destroy_by_condition(procesos_cola_ready->lista_procesos, (void*) (uintptr_t)(pcb_encontrado->pid == pid_buscado), (void*) eliminar_pcb);
    sem_wait(sem_estado_procesos_cola_ready);
    pthread_mutex_unlock(mutex_procesos_cola_ready);

    log_info(logger, "Proceso con PID: %d ha sido eliminado.", pcb_encontrado->pid);
    log_info(logger, "## Finaliza el proceso %d", pcb_encontrado->pid);
}

void notificar_memoria_fin_proceso(int pid) {

    t_peticion *peticion = malloc(sizeof(t_peticion));
        peticion->tipo = PROCESS_EXIT_OP;
        peticion->proceso = obtener_pcb_por_pid(pid); 
        peticion->hilo = NULL; // No aplica en este caso  
        encolar_peticion_memoria(peticion);
        sem_wait(sem_estado_respuesta_desde_memoria);
    if (peticion->respuesta_exitosa) {
        log_info(logger, "Finalización del proceso con PID %d confirmada por la Memoria.", pid);
    } else {
        log_info(logger, "Error al finalizar el proceso con PID %d en la Memoria.", pid);
        }   
}

//Ademas de eliminar el hilo de acá me tengo que asegurar de que memoria destruya las estructuras de los mismos
void eliminar_hilo_de_cola_fifo_prioridades(t_tcb* tcb_asociado) {
    pthread_mutex_lock(mutex_hilos_cola_ready);
    
    for (int i = 0; i < list_size(hilos_cola_ready->lista_hilos); i++) {
        t_tcb *hilo = list_get(hilos_cola_ready->lista_hilos, i);
        if (hilo->pid == tcb_asociado->pid) {
            list_remove(hilos_cola_ready->lista_hilos, i);
            i--;
            encolar_en_exit(hilo); //se agrega a la cola de exit
            sem_wait(sem_estado_hilos_cola_ready);
            break;
        }
    }
    pthread_mutex_unlock(mutex_hilos_cola_ready);
}

void eliminar_hilo_de_cola_multinivel(t_tcb* tcb_asociado) {
    pthread_mutex_lock(mutex_colas_multinivel);

    // Iterar sobre cada nivel de prioridad
    for (int i = 0; i < list_size(colas_multinivel->niveles_prioridad); i++) {
        t_nivel_prioridad *nivel = list_get(colas_multinivel->niveles_prioridad, i);

        pthread_mutex_lock(mutex_colas_multinivel);
        for (int j = 0; j < list_size(nivel->cola_hilos->lista_hilos); j++) {
            t_tcb *hilo = list_get(nivel->cola_hilos->lista_hilos, j);
            if (hilo->pid == tcb_asociado->pid) {
                list_remove(nivel->cola_hilos->lista_hilos, j);
                j--;
                encolar_en_exit(hilo);//se agrega a la cola de exit
                sem_wait(sem_estado_multinivel);
                break;
            }
        }
        pthread_mutex_unlock(mutex_colas_multinivel);
    }
    pthread_mutex_unlock(mutex_colas_multinivel);
}

void encolar_en_exit(t_tcb * hilo){
    pthread_mutex_lock(mutex_hilos_cola_exit);
    list_add(hilos_cola_exit->lista_hilos, hilo);
    pthread_mutex_unlock(mutex_hilos_cola_exit);
    sem_post(sem_estado_hilos_cola_exit);
}

//HILOS

void THREAD_CREATE(FILE* archivo_instrucciones, int prioridad) {
    int tid = ++ultimo_tid;
    pthread_mutex_lock(mutex_procesos_cola_ready);
    t_tcb* nuevo_tcb = crear_tcb(proceso_actual->pid, tid, prioridad);
    cambiar_estado(nuevo_tcb, READY);
    nuevo_tcb->instrucciones = interpretarArchivo(archivo_instrucciones);
    list_add(proceso_actual->listaTCB, nuevo_tcb);
    encolar_hilo_corto_plazo(nuevo_tcb);
    pthread_mutex_unlock(mutex_procesos_cola_ready);
    notificar_memoria_creacion_hilo(nuevo_tcb);
    log_info(logger, "## (%d:%d) Se crea el Hilo - Estado: READY", proceso_actual->pid, tid);
}

void THREAD_JOIN(int tid_a_esperar) {
    t_tcb* hilo_a_esperar = obtener_tcb_por_tid(tid_a_esperar);

    if (hilo_a_esperar == NULL || hilo_a_esperar->estado == EXIT) {
        log_info(logger, "THREAD_JOIN: Hilo TID %d no encontrado o ya finalizado.", tid_a_esperar);
        return;
    }

    //trabajar con los semáforos
    cambiar_estado(hilo_actual, BLOCK);
    agregar_hilo_a_lista_de_espera(hilo_a_esperar, hilo_actual);
    //agregar una interrupcion a CPU para que deje de ejecutarlo y ejecute el siguiente hilo de la cola
    enviar_a_cpu_interrupt(hilo_actual->tid, THREAD_JOIN_OP);

    // Obtengo el PCB correspondiente al hilo actual
    t_pcb* pcb_hilo_actual = obtener_pcb_por_pid(hilo_actual->pid);
    if (pcb_hilo_actual != NULL) {
        log_info(logger, "## (%d:%d) - Bloqueado por: THREAD_JOIN", pcb_hilo_actual->pid, hilo_actual->tid);
    } else {
        log_warning(logger, "No se encontró el PCB para el TID %d.", hilo_actual->tid);
    }

    log_info(logger, "THREAD_JOIN: Hilo TID %d se bloquea esperando a Hilo TID %d.", hilo_actual->tid, tid_a_esperar);
}

void finalizar_hilo(t_tcb* hilo) {
    cambiar_estado(hilo, EXIT);
    t_list* lista_hilos_en_espera = obtener_lista_de_hilos_que_esperan(hilo);
    for (int i = 0; i < list_size(lista_hilos_en_espera); i++) {
        t_tcb* hilo_en_espera = list_get(lista_hilos_en_espera, i);
        cambiar_estado(hilo_en_espera, READY);
    }
}

void THREAD_CANCEL(int tid_hilo_a_cancelar) { // Esta sys recibe el tid solamente del hilo a cancelar
    t_tcb* hilo_a_cancelar = obtener_tcb_por_tid(tid_hilo_a_cancelar);
    if (hilo_a_cancelar == NULL) {
        log_warning(logger, "TID %d no existe o ya fue finalizado.", tid_hilo_a_cancelar);
        return;
    }

    hilo_a_cancelar->estado = EXIT;
    log_info(logger, "Hilo TID %d movido a EXIT.", tid_hilo_a_cancelar);
    t_pcb * proceso = obtener_pcb_por_tid(tid_hilo_a_cancelar);
    for (int i = 0; i < list_size(proceso->listaTCB); i++) {
        t_tcb *hilo = list_get(hilos_cola_ready->lista_hilos, i);
        if (hilo->pid == tid_hilo_a_cancelar) {
            list_remove(proceso->listaTCB, i);
            i--;
            break;
        }
    }

    log_info(logger, "## (%d) Finaliza el hilo", hilo_a_cancelar->tid);

    // Elimina el hilo de la cola correspondiente y lo encola en la cola de EXIT
    if (strcmp(algoritmo, "FIFO") == 0 || strcmp(algoritmo, "PRIORIDADES")) {
            eliminar_hilo_de_cola_fifo_prioridades(hilo_a_cancelar);
        } else if (strcmp(algoritmo, "CMN") == 0) {
            eliminar_hilo_de_cola_multinivel(hilo_a_cancelar);
        } else {
            printf("Error: Algoritmo no reconocido.\n");
        }

    notificar_memoria_fin_hilo(hilo_a_cancelar);
    eliminar_tcb(hilo_a_cancelar);
}

void notificar_memoria_creacion_hilo(t_tcb* hilo) {
    t_peticion *peticion = malloc(sizeof(t_peticion));
        peticion->tipo = THREAD_CREATE_OP;
        peticion->proceso = NULL; // No aplica en este caso
        peticion->hilo = hilo; 
        encolar_peticion_memoria(peticion);
        wait(sem_estado_respuesta_desde_memoria);
    if (peticion->respuesta_exitosa) {
        log_info(logger, "Información sobre la creacion del hilo con TID %d enviada a Memoria.", hilo->tid);
    } else {
        log_info(logger, "Error al informar sobre la creacion del hilo con TID %d a Memoria.", hilo->tid);
        }   
}

void notificar_memoria_fin_hilo(t_tcb* hilo) {
    t_peticion *peticion = malloc(sizeof(t_peticion));
    peticion->tipo = THREAD_EXIT_OP;
    peticion->proceso = NULL; // No aplica en este caso
    peticion->hilo = hilo; 
    encolar_peticion_memoria(peticion);
    wait(sem_estado_respuesta_desde_memoria);
    if (peticion->respuesta_exitosa) {
        log_info(logger, "Finalización del hilo con TID %d confirmada por la Memoria.", hilo->tid);
    } else {
        log_info(logger, "Error al finalizar el hilo con TID %d en la Memoria.", hilo->tid);
        }   
}

void eliminar_tcb(t_tcb* hilo) {
    if (hilo == NULL) {
        return;
    }

    // Implementar lógica para liberar otros recursos?
    free(hilo);  // Libero el TCB
}

void THREAD_EXIT() {// No recibe ningún parámetro, trabaja con hilo_actual
    t_tcb* hilo_a_salir = hilo_actual;
    if (hilo_a_salir == NULL) {
        log_warning(logger, "No se encontró el TID %d para finalizar el hilo.", hilo_a_salir->tid);
        return;
    }

    hilo_a_salir->estado = EXIT;

    // Obtengo el PCB correspondiente al hilo
    t_pcb* pcb_hilo_a_salir = obtener_pcb_por_tid(hilo_a_salir->tid);
    if (pcb_hilo_a_salir == NULL) {
        log_warning(logger, "No se encontró el PCB para el TID %d.", hilo_a_salir->tid);
        return;
    }
    // Elimina el hilo de la cola correspondiente y lo encola en la cola de EXIT
    if (strcmp(algoritmo, "FIFO") == 0 || strcmp(algoritmo, "PRIORIDADES")) {
            eliminar_hilo_de_cola_fifo_prioridades(hilo_a_salir);
        } else if (strcmp(algoritmo, "CMN") == 0) {
            eliminar_hilo_de_cola_multinivel(hilo_a_salir);
        } else {
            printf("Error: Algoritmo no reconocido.\n");
        }

    log_info(logger, "Hilo TID %d finalizado.", hilo_a_salir->tid);

    notificar_memoria_fin_hilo(hilo_a_salir);
    eliminar_tcb(hilo_a_salir);
}

void IO(float milisec, int tcb_id)
{
    pthread_mutex_lock(mutex_colaIO);
    t_uso_io *peticion;
    peticion->milisegundos = milisec;
    peticion->tid = tcb_id;
    list_add(colaIO->lista_io, peticion);
    pthread_mutex_unlock(mutex_colaIO);
 //log_info(logger, "## (%d:%d) finalizó IO y pasa a READY", hilo_actual->pid, hilo_actual->tid);   
}


void MUTEX_CREATE(char* nombre_mutex,t_pcb *pcb) {
    
    for (int i = 0; i < list_size(pcb->listaMUTEX); i++) {
        t_mutex* mutex = list_get(pcb->listaMUTEX, i);
        if (strcmp(mutex->nombre, nombre_mutex) == 0) {
            log_warning(logger, "El mutex %s ya existe.", nombre_mutex);
            return;
        }
    }

    t_mutex* nuevo_mutex = malloc(sizeof(t_mutex));
    nuevo_mutex->nombre = strdup(nombre_mutex);
    nuevo_mutex->estado = 0; // Mutex empieza libre
    nuevo_mutex->hilos_esperando = list_create();

    list_add(pcb->listaMUTEX, nuevo_mutex);
    log_info(logger, "Mutex %s creado exitosamente.", nombre_mutex);
}

void MUTEX_LOCK(char* nombre_mutex, t_tcb* hilo_actual,t_pcb *pcb) {
    t_mutex* mutex_encontrado = NULL;

    for (int i = 0; i < list_size(pcb->listaMUTEX); i++) {
        t_mutex* mutex = list_get(pcb->listaMUTEX, i);
        if (strcmp(mutex->nombre, nombre_mutex) == 0) {
            mutex_encontrado = mutex;
            break;
        }
    }

    if (mutex_encontrado == NULL) {
        log_warning(logger, "El mutex %s no existe.", nombre_mutex);
        return;
    }

    if (mutex_encontrado->estado == 0) {
        mutex_encontrado->estado = 1; // Bloqueo
        log_info(logger, "Mutex %s adquirido por el hilo TID %d.", nombre_mutex, hilo_actual->tid);
    } 

    // Si el mutex está bloqueado, el hilo entra en espera
    else {
        log_info(logger, "Mutex %s ya está bloqueado. El hilo TID %d entra en espera.", nombre_mutex, hilo_actual->tid);
        list_add(mutex_encontrado->hilos_esperando, hilo_actual);
        cambiar_estado(hilo_actual, BLOCK);
    }
}

void MUTEX_UNLOCK(char* nombre_mutex,t_pcb *pcb) {
    t_mutex* mutex_encontrado = NULL;

    for (int i = 0; i < list_size(pcb->listaMUTEX); i++) {
        t_mutex* mutex = list_get(pcb->listaMUTEX, i);
        if (strcmp(mutex->nombre, nombre_mutex) == 0) {
            mutex_encontrado = mutex;
            break;
        }
    }

    if (mutex_encontrado == NULL) {
        log_warning(logger, "El mutex %s no existe.", nombre_mutex);
        return;
    }

    if (mutex_encontrado->estado == 1) {
        mutex_encontrado->estado = 0; // Liberar el mutex
        log_info(logger, "Mutex %s liberado.", nombre_mutex);

        // Despertar un hilo de la lista de espera, si hay alguno
        if (list_size(mutex_encontrado->hilos_esperando) > 0) {
            t_tcb* hilo_despertar = list_remove(mutex_encontrado->hilos_esperando, 0); // Quitar el primer hilo
            cambiar_estado(hilo_despertar, READY); // Cambiar su estado a READY
            log_info(logger, "Hilo TID %d ha sido despertado y ahora tiene el mutex %s.", hilo_despertar->tid, nombre_mutex);
        }
    } else {
        log_warning(logger, "El mutex %s ya estaba libre.", nombre_mutex);
    }
}

void DUMP_MEMORY(int pid) {
    log_info(logger, "=== DUMP DE MEMORIA ===");

    log_info(logger, "Envio un mensaje a memoria que vacie el proceso con el pid %d",pid);

    t_peticion *peticion = malloc(sizeof(t_peticion));
    peticion->tipo = DUMP_MEMORY_OP;
    peticion->proceso = obtener_pcb_por_pid(pid);
    peticion->hilo = NULL; 
    encolar_peticion_memoria(peticion);
    wait(sem_estado_respuesta_desde_memoria);
    log_info(logger, "Espero respuesta de memoria");

    log_info(logger, "FIN DEL DUMP DE MEMORIA");
}

void element_destroyer(void* elemento) 
{
    t_instruccion* instruccion = (t_instruccion*)elemento;
    free(instruccion->ID_instruccion);
    free(instruccion);
}

// interpretar un archivo y crear una lista de instrucciones
t_list* interpretarArchivo(FILE* archivo) 
{
    if (archivo == NULL) {
        perror("Error al abrir el archivo");
        return NULL;
    }

    char linea[100];
    t_list* instrucciones = list_create();
    if (instrucciones == NULL) {
        perror("Error de asignación de memoria");
        return NULL;
    }

    while (fgets(linea, sizeof(linea), archivo) != NULL) {
        linea[strcspn(linea, "\n")] = 0; 

        t_instruccion* instruccion = malloc(sizeof(t_instruccion));
        if (instruccion == NULL) {
            perror("Error de asignación de memoria para la instrucción");
            list_destroy_and_destroy_elements(instrucciones, element_destroyer);
            return NULL;
        }

        char* token = strtok(linea, " ");
        if (token != NULL) {
            instruccion->ID_instruccion = strdup(token);

            // Si la instrucción es "SALIR", no asignar parámetros
            if (strcmp(instruccion->ID_instruccion, "SALIR") == 0) {
                instruccion->parametros_validos = 0;
                instruccion->parametros[0] = 0;
                instruccion->parametros[1] = 0;
            } else {
                instruccion->parametros_validos = 1;
                for (int i = 0; i < 2; i++) {
                    token = strtok(NULL, " ");
                    if (token != NULL) {
                        instruccion->parametros[i] = atoi(token);
                    } else {
                        instruccion->parametros[i] = 0;
                    }
                }
            }

            list_add(instrucciones, instruccion);
        } else {
            free(instruccion);
        }
    }

    return instrucciones;
}

void liberarInstrucciones(t_list* instrucciones) 
{
    list_destroy_and_destroy_elements(instrucciones, element_destroyer);
}

t_tcb* obtener_tcb_por_tid(int tid) {
    for (int i = 0; i < list_size(lista_global_tcb); i++) {
        t_tcb* hilo = list_get(lista_global_tcb, i);
        if (hilo->tid == tid) {
            return hilo;
        }
    }
    return NULL;
}

t_tcb* obtener_tcb_actual() {
    return hilo_actual;
}

void agregar_hilo_a_lista_de_espera(t_tcb* hilo_a_esperar, t_tcb* hilo_actual) {
    if (hilo_a_esperar->lista_espera == NULL) {
        hilo_a_esperar->lista_espera = list_create();
    }
    list_add(hilo_a_esperar->lista_espera, hilo_actual);
}

t_list* obtener_lista_de_hilos_que_esperan(t_tcb* hilo) {
    return hilo->lista_espera;
}