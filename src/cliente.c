#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <errno.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <signal.h>
#include <sys/ioctl.h>	  
#include <sys/types.h>	 
#include <sys/socket.h>	
#include <unistd.h>
#include <time.h>

#define TAM_BUF 18
#define MARCA 0x1E

int sock_cliente, sock_servidor, len,seq=1;
unsigned char saida[TAM_BUF], entrada[TAM_BUF];
unsigned char marcap=0,tamp=0,seqp=0,tipop=0; //guardam info do (p)acote decodificado
	struct ifreq ifrequest;
	struct sockaddr_ll dest;
FILE *arq = NULL, *mus = NULL;


//	Todas as mensagens devem ter o seguinte formato:
//	BUFF	[     0      |           1           |2       ...    16 |  17 ]
//		[ marca	| tamanho | sequencia | tipo | DADOS		| CRC ]
//		   6bit	   4bit       2bit	4bit	15bytes		  1byte



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

/* aguarda confirmacao do envio */
int confirma(){
	time_t i=0,t=0;
	int erro;
	erro=recv( sock_cliente, (char *) &entrada, sizeof(entrada),  MSG_DONTWAIT);
	decodPacote();
	if ((erro > 0) && (marcap==MARCA) && (seqp==(seq%4)) ) return 1; //recebeu sem problemas

	t=time(NULL);
	for(i=t;(((i-t)<5) && (erro==-1)) || (marcap!=MARCA);){
		erro=recv( sock_cliente, (char *) &entrada, sizeof(entrada), MSG_DONTWAIT);
		decodPacote();
		i=time(NULL);
	}

	if((i-t)>=5) {
		printf("Timeout atingido, reenviando pacote.\n");
		envia();
	}
	
	return 1;
}

/* imprime o erro ocorrido */
void errou(unsigned char id){

	if(id==0)
		printf("ERRO: Arquivo ou diretorio inexistente.\n");
	if(id==1)
		printf("ERRO: Nao ha' permissao para executar esta operacao.\n");
	if(id==2)
		printf("ERRO: Nao ha' espaco suficiente em disco.\n");

}

/* recebe pacotes e escreve no arquivo ou stdout */
void recebe(FILE *output){
	int fim=0, erro=0, print=0;
	unsigned int dload=0;
	montaPacote(0,2); //NACK por default
	memset(&saida[2], '\0', TAM_BUF-2 ); 

	entrada[0] = 0x88 << 2;
	entrada[1] = 4 << 2;
	marcap=tamp=seqp=tipop=0;

	while (!fim) {

		erro=recv(sock_cliente,(char *) &entrada,sizeof(entrada),0x00);
		decodPacote();
		if(erro > 0)
		if((marcap == MARCA) && (seqp == (seq%4)) && (!restoCRC(entrada,18))) {
			if((tipop != 0x0F) && (tipop != 0x0E)){
				if(!print && (output == arq)){
					printf("[");
					print=1;
				}
				if( (((dload*16)%131072) == 0) && (output == arq)){
					printf("=");
					fflush(stdout);
				}
				dload++;
				fwrite(&entrada[2],1,tamp,output);
			} else if (tipop == 0x0E)
				errou(entrada[2]);
			montaPacote(0,1); //ACK
			seq++;		
		} 
		memset(&entrada[2], '\0', TAM_BUF-2 ); 
		saida[17] = 0;
		saida[17] = restoCRC(saida,18); 
		if (erro !=0) 
			sendto(sock_cliente,  saida, TAM_BUF,  0,  (struct sockaddr*) &dest, len );
		if((marcap == MARCA) && (tipop == 0x0F) && (seqp == ((seq-1)%4)))
			fim=1;
	}

}

/* envia com o devido byte de CRC*/
int envia(){
	int erro;
	saida[17] = 0;
	saida[17] = restoCRC(saida,18);
	erro=sendto(sock_cliente,  saida, TAM_BUF,  0,  (struct sockaddr*) &dest, len );
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

/* pega conteudo de arquivo e coloca no buffer */
void put(char *cmd){
	int tam;
	printf("Enviando o arquivo: %s\n",&cmd[4]);	
	arq=fopen(&cmd[4],"rb");
	for(; !feof(arq); seq++){
		memset(&saida[2],'\0',TAM_BUF-3);
		tam=fread(&saida[2], 1,TAM_BUF-3, arq);
		montaPacote(tam,0x0D);
		if(!envia()) return;
	}
	fclose(arq);
	arq = NULL;
	// montar pacote final
	montaPacote(0,0x0F);
	sendto(sock_cliente,  saida, TAM_BUF,  0,  (struct sockaddr*) &dest, len );
	seq++;
}



void remoto(){
	char cmd[16], tam=0;
	len = sizeof(struct sockaddr_ll);
	strcpy(cmd,&saida[2]);

	if((saida[2] == 'c') && (saida[3] == 'd'))
		montaPacote(strlen(&saida[2]),0x00);
	
	if(saida[2] == 'l' && saida[3] == 's')
		montaPacote(strlen(&saida[2]),0x03);

	if((saida[2] == 'p') && (saida[3] == 'u')&& (saida[4] == 't'))
		montaPacote(strlen(&saida[2]),0x05);

	if((saida[2] == 'g') && (saida[3] == 'e') && (saida[4] == 't'))
		montaPacote(strlen(&saida[2]),0x06);
	
	if(!envia()) return;
	seq++;
	montaPacote(0,0x0F);
	sendto(sock_cliente,  saida, TAM_BUF,  0,  (struct sockaddr*) &dest, len );
	//envia();
	seq++;

	if((cmd[0] == 'p') && (cmd[1] == 'u')&& (cmd[2] == 't')){
		put(cmd);
	} else if((cmd[0] == 'g') && (cmd[1] == 'e')&& (cmd[2] == 't')){
		arq=fopen(&cmd[4],"wb");
		if(arq == NULL || arq == 0){
			perror("fopen");
			exit(1);
		}
		printf("Recebendo arquivo: %s\n",&cmd[4]);
		fflush(stdout);
		recebe(arq);
		if(tipop!=0xE) printf("]\n");
		fclose(arq);
		arq=NULL;
		if(tipop!=0xE) printf("Transferencia completa. \n");
	} else
		recebe(stdout);

}

void local(char *str){
	char cmd[16];

	if((str[0] == 'l') && (str[1] == 's')){
		strcpy(cmd, str);
		system(cmd);
	}
	else if((str[0] == 'c') && (str[1] == 'd')){
		if( access(&str[3], F_OK ) != -1 ){
			if( access(&str[3], R_OK ) != -1 ){
				strcpy(cmd, &str[3]);
				chdir(cmd);
				system("pwd");
		    	} else errou(1);
		} else errou(0);
	}

	else if (!strcmp(str,"clear")){
		system ("clear");
	}

	else if((str[0] == 'g') && (str[1] == 'e') && (str[2] == 't')){
		if( access(&str[3], F_OK ) != -1 ){
			if( access(&str[3], R_OK ) != -1 ){
				strcpy(cmd, "cat ");
				strcat(cmd, &str[4]);
				system(cmd);
		    	} else errou(1);
		} else errou(0);
	}
	else if((str[0] == 'p') && (str[1] == 'u')&& (str[2] == 't')){
		strcpy(cmd, "touch ");
		strcat(cmd, &str[4]);
		system(cmd);
	}
	else printf("Opcao invalida. Digite \"sair\" caso queira fechar o programa.\n");

}

void terminal() {
	char op, cmd[200];
	do {
		strcpy(cmd,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"); //para ter mais de 15char
		while(strlen(cmd)>15){
			memset(cmd,'\0',TAM_BUF-2);
			op=0;
			rewind(stdin);
			printf("\nExecute um comando com o devido prefixo | (l)ocal ou (r)emoto\n");
			printf("$ ");
			op=getchar();
			gets(cmd);
			if(strlen(cmd)>15)
				printf("\nO comando nao pode ultrapassar 15 caracteres");
		}
		memset(&saida[2],'\0',TAM_BUF-2);
		strcpy(&saida[2],cmd);
		if ((op == 's') && !strcmp(&saida[2],"air")){
			strcpy(&saida[2],"sair");
			montaPacote(strlen(&saida[2]),0x0F);
			seq++;
			sendto(sock_cliente,  saida, TAM_BUF,  0,  (struct sockaddr*) &dest, len );
		}
		else if (op=='l')
			local(&saida[2]);
		else if (op=='r')
			remoto(sock_cliente);
		else
			printf("Opcao invalida. Digite \"sair\" caso queira fechar o programa.\n");
	
	} while( strcmp(&saida[2], "sair") );

	return;
}


int main(int argc,char *argv[]) {

        sock_cliente = socket (PF_PACKET, SOCK_RAW, 0);

        if (sock_cliente == -1){
                printf("Erro ao abrir socket.\n");
                exit (1);
        }

        memset (&ifrequest, 0, sizeof(struct ifreq));
        memcpy (ifrequest.ifr_name, "eth0" , 5);

        ioctl (sock_cliente, SIOCGIFINDEX, &ifrequest);
        memset (&dest, 0, sizeof(dest));
        dest.sll_family = AF_PACKET;
        dest.sll_ifindex = ifrequest.ifr_ifindex;
        dest.sll_protocol = htons(ETH_P_ALL);

        if (bind(sock_cliente, (struct sockaddr *)&dest, sizeof(dest)) < 0){
                printf("Erro ao atribuir endereco ao socket.\n");
                exit (1);
        }

        struct packet_mreq mrequest;
        memset (&mrequest, 0, sizeof(mrequest));
        mrequest.mr_ifindex = ifrequest.ifr_ifindex;
        mrequest.mr_type = PACKET_MR_PROMISC;

        setsockopt (sock_cliente, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mrequest, sizeof(mrequest));
	
	terminal();

	close(sock_cliente);
	return(0);
}
