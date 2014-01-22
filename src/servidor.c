#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h> 
#include <netpacket/packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>


#if !defined(IPVERSION)
#define IPVERSION 4
#endif

#ifndef _GNU_SOURCE
  #define _GNU_SOURCE
#endif

#define ARQUIVO ".log.tmp"
#define TAM_BUF 18
#define MARCA 0x1E

unsigned char entrada[TAM_BUF], saida[TAM_BUF], aux[TAM_BUF]; // buffer de recepcao

int sock_servidor, on, len, seq=1;
struct ifreq ifrequest;
struct sockaddr_ll dest;
char flagerro;
unsigned char marcap,tamp,seqp,tipop; //guardam info do (p)acote decodificado
FILE *arq = NULL;


//Prototipos
unsigned char restoCRC(char *, int);
void montaPacote(unsigned char, unsigned char);
void decodPacote();
void criasocket ();
int confirma();
int envia();


unsigned char restoCRC(char *cmdln, int tam){
        int i, j;
        char resto = 0;
    
        for (i=0; i<tam; i++){
                resto ^= cmdln[i];
                for (j=0; j<8; j++)
                        if (resto & 0x80 != 0)
                                resto = (resto << 1) ^ 0x31;
                        else
                                resto = resto << 1;
        }   
        return resto;
}


void montaPacote(unsigned char tam, unsigned char tipo){

	saida[0] = MARCA << 2;
	saida[0] = saida[0] | (tam >> 2);
	saida[1] = tam << 6;
	saida[1] = saida[1] | ((seq%4) << 4);
	saida[1] = saida[1] | tipo;
	saida[(tam+2)] = '\0';
	saida[17] = 0x0;
	saida[17] = restoCRC(saida,18);
}


void decodPacote(){

	marcap = entrada[0] >> 2;
	tamp = ((entrada[0] ^ (MARCA << 2)) << 2) | (entrada[1] >> 6);
	seqp = ((tamp << 6) ^ entrada[1]) >> 4;
	seqp = seqp << 6;
	seqp = seqp >> 6;
	tipop = ((tamp << 6) | (seqp << 4)) ^ entrada[1];

}


void criasocket (){
	len = sizeof(struct sockaddr_ll);
        sock_servidor = socket (PF_PACKET, SOCK_RAW, 0);

        if (sock_servidor == -1){
                printf("Erro ao abrir socket.\n");
                exit (1);
        }

        memset (&ifrequest, 0, sizeof(struct ifreq));
        memcpy (ifrequest.ifr_name, "eth0" , 5);

        ioctl (sock_servidor, SIOCGIFINDEX, &ifrequest);
        memset (&dest, 0, sizeof(dest));
        dest.sll_family = AF_PACKET;
        dest.sll_ifindex = ifrequest.ifr_ifindex;
        dest.sll_protocol = htons(ETH_P_ALL);

        if (bind(sock_servidor, (struct sockaddr *)&dest, sizeof(dest)) < 0){
                printf("Erro ao atribuir endereço ao socket.\n");
                exit (1);
        }

        struct packet_mreq mrequest;
        memset (&mrequest, 0, sizeof(mrequest));
        mrequest.mr_ifindex = ifrequest.ifr_ifindex;
        mrequest.mr_type = PACKET_MR_PROMISC;

        setsockopt (sock_servidor, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mrequest, sizeof(mrequest));
}


void recebe(char *cmdln, char *cmd, int op){
	int fim=0, erro=0;
	montaPacote(0,2); //NACK por default
	memset(&saida[2], '\0', TAM_BUF-2 ); // campo vazio

	entrada[0] = 0x88 << 2;
	entrada[1] = 4 << 2;
	marcap=tamp=seqp=tipop=0;
	while (!fim) {

		erro=recv(sock_servidor,(char *) &entrada,sizeof(entrada),0x00);
		//printf ("Sequencia:%d\n", seq%4);
		decodPacote();	
		//printf("\nmarcap %x, tam %d, seq %d, tipo %d\n",marcap,tamp,seqp,tipop);
		if(erro > 0)
		if((marcap == MARCA) && (seqp == (seq%4)) && (!restoCRC(entrada,18))) {
			if((tipop!=0x0F)){
				if(!op)
					strcpy(cmdln, &entrada[2]);			
				else{
					fwrite(&entrada[2],1,tamp,arq);
					//printf("Escrevendo no arquivo: %s\n",cmdln);
				}
				if(tipop == 0 || tipop == 3 || tipop == 6 || tipop == 5)  
				*cmd=tipop;
			}
			montaPacote(0,1); //ACK
			seq++;
		} 
		memset(&entrada[2], '\0', TAM_BUF-2 ); // campo vazio
		saida[17] = 0;
		saida[17] = restoCRC(saida,18); 
		if (erro !=0)
			sendto(sock_servidor,  saida, TAM_BUF,  0,  (struct sockaddr*) &dest, len );
		if((marcap == MARCA) && (tipop == 0x0F) && (seqp == ((seq-1)%4)))
			fim=1;
		if(!strcmp(&entrada[2],"sair"))
			exit(1);
	}
}

/* aguarda confirmação do envio */
int confirma(){
	time_t i=0,t=0;
	int erro;
	erro=recv( sock_servidor, (char *) &entrada, sizeof(entrada),  MSG_DONTWAIT);
	decodPacote();
	if ((erro > 0) && (marcap==MARCA) && (seqp==(seq%4)) ) return 1; //recebeu sem problemas

	t=time(NULL);
	for(i=t;(((i-t)<5) && (erro==-1)) || (marcap!=MARCA);){
		erro=recv( sock_servidor, (char *) &entrada, sizeof(entrada), MSG_DONTWAIT);
		decodPacote();
		i=time(NULL);
	}

	if((i-t)>=5) {
		printf("Timeout atingido, reenviando pacote.\n");
		envia();
	}
	
	return 1;
}

/* envia com o devido byte de CRC*/
int envia(){
	int erro;
	saida[17] = 0;
	saida[17] = restoCRC(saida,18);
	erro=sendto(sock_servidor,  saida, TAM_BUF,  0,  (struct sockaddr*) &dest, len );
	if( erro < 0){
		perror("sendto");
		return 0;
	 }
	else{
		if(!confirma()) return 0;
		decodPacote();
		while(tipop==2){ //caso receba um NACK
			decodPacote();
			if(envia()) return 1; else return 0;
		}
		if(tipop==1){
			//printf("ACK!\n");	
			return 1;
		}
		return 1;
	}
}

int put(){
	int tam;

	for(; !feof(arq); seq++){
		memset(&saida[2],'\0',TAM_BUF-3);
		tam=fread(&saida[2], 1,TAM_BUF-3, arq);
		if(flagerro) montaPacote(tam,0x0E);
		else montaPacote(tam,0x0D);
		if(!envia()) return;
	}
	flagerro = 0;
	fclose(arq);
	arq = NULL;
	// montar pacote final
	montaPacote(0,0x0F);
	sendto(sock_servidor,  saida, TAM_BUF,  0,  (struct sockaddr*) &dest, len );
	seq++;
}

void senderro(char erro){

	arq = fopen(ARQUIVO, "w");
	if(arq == NULL) {
		perror("fopen");
		exit(1);
	}
	flagerro=1;
	memset(&saida[2],'\0',TAM_BUF-2);
	montaPacote(0,0x0E);
	fprintf(arq,"%c",erro);
	fclose(arq);

}

void executa(char *string, char cmd){
	char v[TAM_BUF];
	printf("\n%d: $ %s\n",cmd,string);


	if(cmd == 3){ //ls

		if(strlen(string)>3 && (string[3]!='-')) {
			if( access(&string[3], F_OK ) != -1 ) {
				if( access(&string[3], R_OK ) != -1 ){
					strcpy(v, string);
					strcat(v, " > ");
					strcat(v, ARQUIVO);
					system(v);
				} else senderro(1);
			} else senderro(0);
		} else {
			strcpy(v, string);
			strcat(v, " > ");
			strcat(v, ARQUIVO);
			system(v);
		} 
		arq=fopen(ARQUIVO,"r");
		if(arq == NULL || arq ==0){
			perror("fopen");
			exit(1);
		}
	}

	if(cmd == 0){ // cd
		if( access(&string[3], F_OK ) != -1 ){
			if( access(&string[3], R_OK ) != -1 ){
				chdir(&string[3]);
				strcpy(v,"pwd > ");
				strcat(v, ARQUIVO);
				system(v);
		    	} else senderro(1);
		} else senderro(0);

		arq = fopen(ARQUIVO, "r");
		if(arq == NULL) {
			perror("fopen");
			exit(1);
		}
	}

	if(cmd == 6){ //get
		if( access(&string[4], F_OK ) == -1 ) {
			senderro(0);
			arq = fopen(ARQUIVO, "r");
			if(arq == NULL) {
				perror("fopen");
				exit(1);
			}
		} else {
			printf("Enviando arquivo: %s\n",&string[4]);
			arq = fopen(&string[4],"rb");
			if( arq == NULL){
				perror("fopen");
				exit(1);
			} 
		}
	}

	if(cmd == 5){ //put
		if( access(".", W_OK ) != -1 ) {
			arq=fopen(&string[4],"wb");
			if(arq == NULL || arq == 0){
				perror("fopen");
				exit(1);
			}
			printf("Transferindo arquivo: %s\n",&string[4]);
			recebe(v,&cmd,1);
			//seq++;
			fclose(arq);
			arq=NULL;
			printf("Transferência completa. \n");
		} else senderro(1);
	}

}

int main(int argc,char *argv[]){
	char cmdln[16], cmd;

	criasocket();

	do
	{
		printf("\n<?>\n");
		recebe(cmdln,&cmd,0);
		if(cmd == 0 || cmd == 3 || cmd == 6 || cmd == 5)
			executa(cmdln,cmd);

		if(cmd != 5 )
			put();

	} while( strcmp(&entrada[2],"sair") );

	if(arq != NULL)
		if( fclose(arq) != 0 ) perror("fclose");
		else  arq = NULL;
	return(0);
}
